// Draws: the hooks of the three D3D draw functions, which commit the device state and post the host draw built from it.

#include "gpu/guest/draw.h"

#include "gpu/gpu.h"
#include "gpu/guest/addresses.h"
#include "gpu/guest/buffers.h"
#include "gpu/guest/device.h"
#include "gpu/guest/resources.h"
#include "gpu/guest/shaders.h"
#include "gpu/guest/textures.h"
#include "gpu/guest/vertex_input.h"
#include "gpu/renderer/renderer.h"
#include "kernel/kernel.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>

PPC_EXTERN_FUNC(D3D_SET_PENDING_SHADERS);

namespace gpu::guest
{
    namespace
    {
        // GPU primitive types.
        constexpr uint32_t kTriangleList = 4;
        constexpr uint32_t kTriangleFan = 5;
        constexpr uint32_t kTriangleStrip = 6;
        constexpr uint32_t kQuadList = 13;

        // BeginVertices as LTCG specialized it: a fan of 4 vertices of 20 bytes, read through vertex
        // fetch constant 95 with the 8in32 swap. The commit writes that fetch constant only into its
        // packet, so the draw makes its own.
        constexpr uint32_t kInlineVertices = 4;
        constexpr uint32_t kInlineStride = 20;
        constexpr uint32_t kInlineFetch = kStreamFetchBase;

        // Fetch constants: dword 0 holds the type in bits 0..1 (3: vertex). A vertex fetch constant
        // has the address in dword 0, the endian swap in bits 0..1 and the size in dwords in bits
        // 2..25 of dword 1.
        constexpr uint32_t kFetchTypeVertex = 3;

        // A draw copies at most this much vertex data; past it the draw is not plausible.
        constexpr uint64_t kMaxVertexBytes = 64 << 20;

        // RB_MODECONTROL: color and depth (4) or depth only (5).
        constexpr uint32_t kEdramModeMask = 7;
        constexpr uint32_t kEdramDepthOnly = 5;
        // RB_DEPTH_INFO: the depth format is k_24_8_FLOAT.
        constexpr uint32_t kDepthInfoFloat = 0x10000;
        // RB_DEPTHCONTROL.
        constexpr uint32_t kStencilEnable = 0x1;
        constexpr uint32_t kDepthEnable = 0x2;
        constexpr uint32_t kDepthFields = 0x74; // write enable, function
        constexpr uint32_t kStencilFields = 0xFFFFFF80; // back face enable, both faces
        constexpr uint32_t kBackFaceStencil = 0x80;
        // RB_COLORCONTROL: alpha function in bits 0..2, alpha test enable, alpha to mask enable.
        constexpr uint32_t kAlphaFunctionMask = 7;
        constexpr uint32_t kAlphaTestEnable = 0x8;
        constexpr uint32_t kAlphaToMask = 0x10;
        constexpr uint32_t kCompareNever = 0;
        constexpr uint32_t kCompareGreater = 4;
        constexpr uint32_t kCompareGreaterEqual = 6;
        constexpr uint32_t kCompareAlways = 7;
        // PA_SU_SC_MODE_CNTL.
        constexpr uint32_t kCullFields = 0x7; // cull front, cull back, clockwise front face
        constexpr uint32_t kCullFront = 0x1;
        constexpr uint32_t kCullBack = 0x2;
        constexpr uint32_t kPolygonMode = 0x18;
        constexpr uint32_t kPolygonOffsetFront = 0x800;
        constexpr uint32_t kPolygonOffsetBack = 0x1000;
        constexpr uint32_t kMultiPrimitive = 1u << 21; // the reset index splits strips and fans
        // PA_CL_VTE_CNTL: the six viewport scale and offset enables, and the vertex formats (only W
        // given as 1/W, as D3D sets it: X, Y and Z are divided by W).
        constexpr uint32_t kViewportEnables = 0x3F;
        constexpr uint32_t kVertexFormatMask = 0x700;
        constexpr uint32_t kVertexFormatD3D = 0x400;
        // PA_SU_VTX_CNTL: pixel centers at 0.5 (otherwise at integers, as in D3D9).
        constexpr uint32_t kPixelCenterHalf = 0x1;
        // PA_CL_CLIP_CNTL: the user clip plane enables, the pixel shader clip modes, clipping off,
        // the cull-only mode.
        constexpr uint32_t kUserClipPlanes = 0x3F;
        constexpr uint32_t kClipPixelShaderModes = 0xC000;
        constexpr uint32_t kClipDisable = 0x10000;
        constexpr uint32_t kClipCullOnly = 0x20000;

        struct DrawCall
        {
            uint32_t primitive = 0;
            bool indexed = false;
            int32_t baseVertex = 0; // added to every index (DrawVertices: the first vertex)
            uint32_t startIndex = 0;
            uint32_t count = 0;
            uint32_t inlineVertices = 0; // BeginVertices: the vertex area it returned
        };

        // A vertex buffer as a vertex fetch constant describes it.
        struct StreamSource
        {
            uint32_t address = 0; // guest
            uint32_t bytes = 0;
            uint32_t endian = 0;
        };

        // Where a vertex stream of a draw comes from.
        struct StreamPlan
        {
            StreamSource source;
            bool inlineFetch = false; // the BeginVertices quad's vertices
            bool hosted = false;      // the host has a copy of the whole buffer
        };

        // The space of the BeginVertices stream in the vertex data of a draw.
        struct InlineStream
        {
            uint32_t offset = 0;
            uint32_t bytes = 0;
        };

        // The quad of the last BeginVertices, built at the call except for its vertices.
        struct PendingInline
        {
            std::unique_ptr<DrawOp> op;
            uint32_t vertices = 0;           // guest address the title writes
            std::optional<InlineStream> stream; // where its vertices go in op->vertices
            std::thread::id thread;
        };

        // Held while the quad is posted, so that a draw on another thread waits for it (PostInlineDraw).
        // Recursive: the post calls FlushInlineDraw again (Renderer::BeforePost).
        std::recursive_mutex g_pendingMutex;
        PendingInline g_pending;

        void LogUnregistered(uint32_t object, bool pixel)
        {
            static std::mutex mutex;
            static std::unordered_set<uint32_t> logged;
            std::lock_guard lock(mutex);
            if (logged.size() < 16 && logged.insert(object).second)
                Log("gpu: %s shader object %08X was not created through the shader database; its draws are skipped",
                    pixel ? "pixel" : "vertex", object);
        }

        // Copies big-endian dwords as host dwords (a loop the compiler vectorizes).
        void CopySwapped(uint8_t* out, const uint8_t* in, uint32_t bytes)
        {
            for (uint32_t i = 0; i < bytes; i += 4)
            {
                uint32_t value;
                memcpy(&value, in + i, 4);
                value = __builtin_bswap32(value);
                memcpy(out + i, &value, 4);
            }
        }

        // The float constants of one shader stage as the host shaders read them: the device's block in
        // host order, kept current from the dirty mask, which is read before the draw's commit clears
        // it, and the range of the stage's constant ring the draws share until the block changes. Used
        // by the drawing thread; the title draws from one thread at a time, as the device itself
        // requires.
        class ConstantShadow
        {
        public:
            ConstantShadow(ShaderStage stage, uint32_t block, uint32_t dirty) : stage_(stage), block_(block), dirty_(dirty) {}

            // Copies the groups the dirty mask marks (everything the first time).
            void Refresh(uint32_t device)
            {
                uint64_t mask = valid_ ? mem::Load64(device + dirty_) : ~0ull;
                for (; mask != 0; mask &= mask - 1)
                {
                    uint32_t at = (63 - uint32_t(std::countr_zero(mask))) * dev::kConstantGroupBytes;
                    uint8_t group[dev::kConstantGroupBytes];
                    CopySwapped(group, mem::Ptr(device + block_ + at), sizeof(group));
                    if (memcmp(bytes_.data() + at, group, sizeof(group)) == 0)
                        continue;
                    memcpy(bytes_.data() + at, group, sizeof(group));
                    changed_ = true;
                }
                valid_ = true;
            }

            // A range holding registers 0 to registers - 1 as of the last refresh: the last range
            // stored when the block has not changed since and the range holds them, otherwise a new
            // copy. At least one register, so that the stage always has a constant buffer to bind.
            // None when the renderer is stopping.
            std::optional<ConstantRange> Store(Renderer& renderer, uint32_t registers)
            {
                uint32_t bytes = std::max(registers, 1u) * kConstantRegisterBytes;
                if (changed_ || range_.bytes < bytes)
                {
                    std::optional<ConstantSpace> space = renderer.ReserveConstants(stage_, bytes);
                    if (!space)
                        return std::nullopt;
                    memcpy(space->data, bytes_.data(), bytes);
                    range_ = space->range;
                    changed_ = false;
                }
                return range_;
            }

        private:
            ShaderStage stage_;
            uint32_t block_; // offsets in the device
            uint32_t dirty_;
            std::array<uint8_t, kConstantRegisters * kConstantRegisterBytes> bytes_{};
            static_assert(sizeof(bytes_) == dev::kConstantBytes);
            bool valid_ = false;
            bool changed_ = true; // the block changed since range_ was stored
            ConstantRange range_;
        };

        ConstantShadow g_vertexConstants(ShaderStage::Vertex, dev::kVertexConstants, dev::kVertexConstantsDirty);
        ConstantShadow g_pixelConstants(ShaderStage::Pixel, dev::kPixelConstants, dev::kPixelConstantsDirty);

        // Before a draw's commit: the constant shadows take what the commit is about to send, and the
        // fetch constants it marks as changed are noted for the capture.
        void RefreshConstants()
        {
            uint32_t device = GetDevice().Address();
            if (device == 0)
                return;
            g_vertexConstants.Refresh(device);
            g_pixelConstants.Refresh(device);
            NoteChangedFetchConstants(device);
        }

        // The commit of D3DDevice_DrawIndexedVertices and DrawVertices without the packets of the
        // state and of the draw, which nothing executes. It leaves the device and the shader objects
        // as the library's commit does: D3D::SetPending_Shaders runs as it is, on the same condition
        // (it sets RB_MODECONTROL and the program control registers, patches the vertex fetches and
        // stamps the shaders with the open fence; it still writes its own packets and may kick), and
        // the five dirty masks are cleared. Everything else the library's commit calls (the other
        // SetPending functions, the draw packet) writes to the segment and, in a predicated pass, adds
        // the packet to the device's draw list, which only the GPU would walk. SetPending_Shaders
        // reads no mask but the one it is given.
        void Commit(PPCContext& ctx, uint8_t* base, uint32_t device)
        {
            uint64_t state = mem::Load64(device + dev::kStateDirty);
            if ((state & dev::kStateDirtyShaders) != 0)
            {
                ctx.r3.u64 = device;
                ctx.r4.u64 = state;
                D3D_SET_PENDING_SHADERS(ctx, base);
            }
            for (uint32_t i = 0; i < dev::kDirtyMaskCount; i++)
                mem::Store64(device + dev::kDirtyMasks + 8 * i, 0);
        }

        bool ReadStreamSource(uint32_t device, uint32_t fetch, StreamSource& source)
        {
            uint32_t at = device + dev::kFetchConstants + 24 * (fetch / 3) + 8 * (fetch % 3);
            uint32_t d0 = mem::Load32(at);
            uint32_t d1 = mem::Load32(at + 4);
            if ((d0 & 3) != kFetchTypeVertex)
                return false;
            VertexFetch decoded = DecodeVertexFetch(d0, d1);
            source.address = GuestPhysical(decoded.address);
            source.bytes = decoded.bytes;
            source.endian = decoded.endian;
            return true;
        }

        // The indices the draw reads, before the base vertex. False when the index buffer is not
        // covered (logged).
        bool ReadIndices(uint32_t device, const DrawCall& call, CaptureVector<uint32_t>& indices)
        {
            indices.resize(call.count);
            if (!call.indexed)
            {
                for (uint32_t i = 0; i < call.count; i++)
                    indices[i] = i;
                return true;
            }
            uint32_t buffer = mem::Load32(device + dev::kIndexBuffer);
            if (buffer == 0)
            {
                LOG_ONCE("gpu: indexed draw without an index buffer skipped");
                return false;
            }
            uint32_t common = mem::Load32(buffer + res::kCommon);
            bool index32 = (common & res::kIndex32) != 0;
            uint32_t endian = (common >> res::kIndexEndianShift) & 3;
            uint32_t data = mem::Load32(buffer + res::kIndexData);
            if (index32)
            {
                const uint8_t* source = mem::Ptr(data + 4 * call.startIndex);
                for (uint32_t i = 0; i < call.count; i++)
                {
                    uint32_t raw;
                    memcpy(&raw, source + 4 * i, 4);
                    indices[i] = SwapEndian32(raw, endian);
                }
            }
            else
            {
                uint32_t first = data + 2 * call.startIndex;
                Index16Swap swap = DecodeIndex16Swap(endian);
                for (uint32_t i = 0; i < call.count; i++)
                {
                    uint16_t raw;
                    memcpy(&raw, mem::Ptr((first + 2 * i) ^ swap.exchange), 2);
                    indices[i] = swap.swapBytes ? __builtin_bswap16(raw) : raw;
                }
            }
            return true;
        }

        // The triangles of a draw's primitives as a list. reset splits strips and fans. False when
        // the primitive type is not covered (logged).
        bool ToTriangleList(uint32_t primitive, const CaptureVector<uint32_t>& in, std::optional<uint32_t> reset,
            CaptureVector<uint32_t>& out)
        {
            out.clear();
            auto forEachRun = [&](auto&& emit) {
                size_t begin = 0;
                for (size_t i = 0; i <= in.size(); i++)
                {
                    if (i == in.size() || (reset && in[i] == *reset))
                    {
                        emit(begin, i);
                        begin = i + 1;
                    }
                }
            };
            switch (primitive)
            {
            case kTriangleList:
                out.assign(in.begin(), in.begin() + in.size() / 3 * 3);
                return true;
            case kTriangleStrip:
                // Every other triangle is reversed, so that all of them keep the first one's winding.
                forEachRun([&](size_t begin, size_t end) {
                    for (size_t i = begin; i + 2 < end; i++)
                    {
                        bool odd = ((i - begin) & 1) != 0;
                        out.insert(out.end(), { in[odd ? i + 1 : i], in[odd ? i : i + 1], in[i + 2] });
                    }
                });
                return true;
            case kTriangleFan:
                forEachRun([&](size_t begin, size_t end) {
                    for (size_t i = begin + 1; i + 1 < end; i++)
                        out.insert(out.end(), { in[begin], in[i], in[i + 1] });
                });
                return true;
            case kQuadList:
                for (size_t i = 0; i + 3 < in.size(); i += 4)
                    out.insert(out.end(), { in[i], in[i + 1], in[i + 2], in[i], in[i + 2], in[i + 3] });
                return true;
            default:
                LOG_ONCE("gpu: primitive type %u is not covered; its draws are skipped", primitive);
                return false;
            }
        }

        // Polygon offset (PA_SU_SC_MODE_CNTL bits 11 and 12, the scale and offset of each face) as one
        // host depth bias: the front face's unless that face's offset is off or culled, then the back
        // face's. The offset counts in guest depth units: of a 24-bit unorm buffer, or of a 20e4 float
        // one (RB_DEPTH_INFO bit 16), whose 2^-24 / 8 step a shift of 3 turns into the integer unit
        // of a 24-bit buffer. The host buffer is 32-bit float, which scales that integer by the
        // exponent of the primitive's depth instead: exact for depths in [0.5, 1), smaller nearer, as
        // in the reference renderer. The scale counts in 1/16 pixels, the subpixel unit of the slope.
        void ReadPolygonOffset(const Device& device, uint32_t rasterControl, RenderState& state)
        {
            float scale = 0;
            float offset = 0;
            if ((rasterControl & kPolygonOffsetFront) != 0 && (rasterControl & kCullFront) == 0)
            {
                scale = device.RegisterFloat(reg::kPolyOffsetFront);
                offset = device.RegisterFloat(reg::kPolyOffsetFront + 1);
            }
            if ((rasterControl & kPolygonOffsetBack) != 0 && (rasterControl & kCullBack) == 0 && scale == 0 && offset == 0)
            {
                scale = device.RegisterFloat(reg::kPolyOffsetBack);
                offset = device.RegisterFloat(reg::kPolyOffsetBack + 1);
            }
            if (!std::isfinite(scale) || !std::isfinite(offset))
            {
                LOG_ONCE("gpu: a polygon offset that is not finite is not applied");
                return;
            }
            bool float24 = (device.Register(reg::kDepthInfo) & kDepthInfoFloat) != 0;
            double units = float24 ? double(1u << 24) / 8.0 : double((1u << 24) - 1);
            // Clamped so that the shift and the sign stay in range.
            double magnitude = std::min(std::ceil(std::fabs(double(offset)) * units), double(INT32_MAX >> 3));
            int32_t bias = int32_t(magnitude);
            if (float24)
                bias <<= 3;
            state.depthBias = offset < 0 ? -bias : bias;
            // -0 would hash apart from the +0 it equals.
            state.depthBiasSlope = scale != 0 ? scale / 16.0f : 0.0f;
        }

        // The enabled user clip planes (PA_CL_CLIP_CNTL bits 0..5) for the vertex shader's clip
        // distances; the disabled ones stay zero and clip nothing. The game's reflection pass
        // enables plane 0 alone, with the plain clip mode.
        void ReadClipPlanes(const Device& device, uint32_t devAddress, SharedConstants& shared)
        {
            uint32_t clipControl = device.Register(reg::kClipControl);
            uint32_t planes = clipControl & kUserClipPlanes;
            // With clipping disabled no plane applies, whatever enables are left set.
            if (planes == 0 || (clipControl & kClipDisable) != 0)
                return;
            if ((clipControl & kClipPixelShaderModes) != 0 || (clipControl & kClipCullOnly) != 0)
                LOG_ONCE("gpu: user clip plane mode %08X is not covered; the planes clip vertices", clipControl);
            for (uint32_t plane = 0; plane < SharedConstants::kClipPlaneCount; plane++)
            {
                if ((planes & (1u << plane)) == 0)
                    continue;
                for (uint32_t i = 0; i < 4; i++)
                {
                    uint32_t bits = mem::Load32(devAddress + dev::kClipPlanes + 16 * plane + 4 * i);
                    memcpy(&shared.clipPlanes[plane][i], &bits, 4);
                }
            }
        }

        // The fixed-function state, viewport and scissor of the draw. False when the draw is not
        // covered (logged).
        bool ReadState(const Device& device, uint32_t devAddress, DrawOp& op)
        {
            uint32_t depthControl = device.Register(reg::kDepthControl);
            if ((depthControl & kDepthEnable) == 0)
                depthControl &= ~kDepthFields;
            if ((depthControl & kStencilEnable) == 0)
                depthControl &= ~kStencilFields;
            op.state.depthControl = depthControl;
            if ((depthControl & kStencilEnable) != 0)
            {
                uint32_t refMask = device.Register(reg::kStencilRefMask) & 0xFFFFFF;
                op.state.stencilMasks = refMask & ~0xFFu;
                op.stencilReference = uint8_t(refMask);
                if ((depthControl & kBackFaceStencil) != 0 &&
                    (device.Register(reg::kStencilRefMaskBack) & 0xFFFFFF) != refMask)
                    LOG_ONCE("gpu: the back face has its own stencil reference or masks; the front face's are used");
            }
            op.state.blendControl = device.Register(reg::kBlendControl);
            op.state.colorMask = device.Register(reg::kColorMask) & 0xF;
            for (uint32_t i = 0; i < 4; i++)
                op.blendColor[i] = device.RegisterFloat(reg::kBlendColor + i);

            uint32_t rasterControl = device.Register(reg::kRasterControl);
            op.state.cullControl = rasterControl & kCullFields;
            if ((rasterControl & kPolygonMode) != 0)
                LOG_ONCE("gpu: polygon mode %u is drawn filled", (rasterControl >> 3) & 3);
            if ((depthControl & kDepthEnable) != 0)
                ReadPolygonOffset(device, rasterControl, op.state);
            ReadClipPlanes(device, devAddress, op.shared);

            // Alpha test: the pixel shader discards alpha below the threshold, which is GEQUAL;
            // GREATER differs only at the reference itself.
            uint32_t colorControl = device.Register(reg::kColorControl);
            op.state.alphaToCoverage = (colorControl & kAlphaToMask) != 0 ? 1 : 0;
            if ((colorControl & kAlphaTestEnable) != 0)
            {
                float reference = device.RegisterFloat(reg::kAlphaRef);
                switch (colorControl & kAlphaFunctionMask)
                {
                case kCompareAlways:
                    break;
                case kCompareGreater:
                    op.shared.specConstants |= kSpecAlphaTest;
                    op.shared.alphaThreshold = std::nextafter(reference, INFINITY);
                    break;
                case kCompareGreaterEqual:
                    op.shared.specConstants |= kSpecAlphaTest;
                    op.shared.alphaThreshold = reference;
                    break;
                case kCompareNever:
                    return false;
                default:
                    LOG_ONCE("gpu: alpha function %u is not covered; draws with it are not alpha tested",
                        colorControl & kAlphaFunctionMask);
                    break;
                }
            }

            uint32_t viewportControl = device.Register(reg::kViewportControl);
            if ((viewportControl & kViewportEnables) != kViewportEnables ||
                (viewportControl & kVertexFormatMask) != kVertexFormatD3D)
            {
                LOG_ONCE("gpu: viewport control %08X is not covered; draws with it are skipped", viewportControl);
                return false;
            }
            float xScale = device.RegisterFloat(reg::kViewportXScale);
            float xOffset = device.RegisterFloat(reg::kViewportXScale + 1);
            float yScale = device.RegisterFloat(reg::kViewportXScale + 2);
            float yOffset = device.RegisterFloat(reg::kViewportXScale + 3);
            float zScale = device.RegisterFloat(reg::kViewportXScale + 4);
            float zOffset = device.RegisterFloat(reg::kViewportXScale + 5);
            // D3D maps X right and Y down (a negative Y scale), as the host viewport does.
            if (!(xScale > 0.0f) || !(yScale < 0.0f))
            {
                LOG_ONCE("gpu: a mirrored viewport (scale %g, %g) is not covered; draws with it are skipped", xScale,
                    yScale);
                return false;
            }
            float width = 2.0f * xScale;
            float height = -2.0f * yScale;
            op.viewport[0] = xOffset - xScale;
            op.viewport[1] = yOffset + yScale;
            op.viewport[2] = width;
            op.viewport[3] = height;
            op.viewport[4] = std::clamp(zOffset, 0.0f, 1.0f);
            op.viewport[5] = std::clamp(zOffset + zScale, 0.0f, 1.0f);
            // D3D9 pixel centers sit on integer coordinates; the host ones half a pixel further.
            if ((device.Register(reg::kVertexControl) & kPixelCenterHalf) == 0)
            {
                op.shared.halfPixelOffset[0] = 1.0f / width;
                op.shared.halfPixelOffset[1] = -1.0f / height;
            }

            if (mem::Load32(devAddress + dev::kScissorEnable) != 0)
                op.scissor = { int32_t(mem::Load32(devAddress + dev::kScissor)), int32_t(mem::Load32(devAddress + dev::kScissor + 4)),
                    int32_t(mem::Load32(devAddress + dev::kScissor + 8)), int32_t(mem::Load32(devAddress + dev::kScissor + 12)) };
            else
                op.scissor = { 0, 0, INT32_MAX, INT32_MAX };
            return true;
        }

        // The targets of the draw. False when a needed target is missing or not covered.
        bool ReadTargets(const Device& device, DrawOp& op, bool depthOnly)
        {
            if (!depthOnly)
            {
                std::optional<SurfaceDesc> color = device.BoundSurface(dev::kRenderTargets);
                if (!color)
                    return false;
                op.color = *color;
                for (uint32_t i = 1; i < dev::kRenderTargetCount; i++)
                    if (mem::Load32(device.Address() + dev::kRenderTargets + 4 * i) != 0)
                        LOG_ONCE("gpu: multiple render targets are not covered; only render target 0 is drawn");
            }
            if ((op.state.depthControl & (kDepthEnable | kStencilEnable)) != 0)
            {
                std::optional<SurfaceDesc> depth = device.BoundSurface(dev::kDepthStencil);
                if (depth && (depthOnly || depth->samples == op.color.samples))
                    op.depth = *depth;
                else if (depth)
                    LOG_ONCE("gpu: depth-stencil and render target sample counts differ; the depth-stencil is left out");
            }
            return op.color.key != 0 || op.depth.key != 0;
        }

        // The host copy of the index buffer of an indexed draw, when it has one (dynamic indices have
        // none) and holds the indices the draw reads.
        std::optional<BufferKey> HostedIndexBuffer(uint32_t device, const DrawCall& call)
        {
            uint32_t buffer = mem::Load32(device + dev::kIndexBuffer);
            if (buffer == 0)
                return std::nullopt;
            std::optional<IndexData> data = ReadIndexData(buffer);
            if (!data)
                return std::nullopt;
            uint32_t common = mem::Load32(buffer + res::kCommon);
            bool index32 = (common & res::kIndex32) != 0;
            BufferKey key{ data->address, data->bytes, index32 ? BufferFormat::Index32 : BufferFormat::Index16,
                uint8_t((common >> res::kIndexEndianShift) & 3) };
            uint64_t end = (uint64_t(call.startIndex) + call.count) * (index32 ? 4 : 2);
            if (end > key.bytes || !UseHostedBuffer(key))
                return std::nullopt;
            return key;
        }

        // The indices of a draw as a triangle list rebased to the first vertex it reads, and the
        // vertices of each stream from that vertex on: copied, or the range of the host copy of its
        // buffer (plans[i].hosted). inlineStream receives the space of the BeginVertices stream in the
        // vertex data. False when the draw is skipped.
        bool CaptureCopied(const Device& device, uint32_t devAddress, const DrawCall& call, const VertexLayout& layout,
            const std::array<StreamPlan, kMaxVertexStreams>& plans, DrawOp& op, std::optional<InlineStream>* inlineStream)
        {
            // A list is read in place.
            if (call.primitive == kTriangleList)
            {
                DrawCall whole = call;
                whole.count -= whole.count % 3;
                if (!ReadIndices(devAddress, whole, op.indices))
                    return false;
            }
            else
            {
                CaptureVector<uint32_t> indices;
                if (!ReadIndices(devAddress, call, indices))
                    return false;
                std::optional<uint32_t> reset;
                if (call.indexed && (device.Register(reg::kRasterControl) & kMultiPrimitive) != 0)
                    reset = device.Register(reg::kResetIndex);
                if (!ToTriangleList(call.primitive, indices, reset, op.indices))
                    return false;
            }
            if (op.indices.empty())
                return false;
            auto [lowest, highest] = std::minmax_element(op.indices.begin(), op.indices.end());
            int64_t first = int64_t(*lowest) + call.baseVertex;
            uint32_t minIndex = *lowest;
            uint32_t vertexCount = *highest - *lowest + 1;
            if (first < 0)
            {
                LOG_ONCE("gpu: a draw reads vertices before its vertex buffer; it is skipped");
                return false;
            }
            for (uint32_t& index : op.indices)
                index -= minIndex;

            const VertexInput& input = *layout.input;
            for (size_t stream = 0; stream < layout.streamFetch.size(); stream++)
            {
                const StreamSource& source = plans[stream].source;
                // A stream of stride 0 holds one vertex: the extent of its elements.
                uint64_t stride = input.strides[stream];
                uint64_t begin = uint64_t(first) * stride;
                uint64_t bytes = uint64_t(vertexCount) * stride;
                if (stride == 0)
                    for (const VertexElement& element : input.elements)
                        if (element.stream == stream)
                            bytes = std::max<uint64_t>(bytes, element.offset + 4 * element.dwords);
                size_t at = op.vertices.size();
                op.streamOffsets[stream] = uint32_t(at);
                if (plans[stream].inlineFetch && inlineStream != nullptr)
                    *inlineStream = InlineStream{ uint32_t(at), uint32_t(std::min<uint64_t>(bytes, source.bytes)) };
                // A buffer the host keeps a copy of is read from it; other vertices are copied.
                if (plans[stream].hosted && begin + bytes <= source.bytes)
                {
                    op.hosted[stream] = { GpuAddress(source.address), source.bytes, uint32_t(begin), uint32_t(bytes) };
                    continue;
                }
                if (at + bytes > kMaxVertexBytes)
                {
                    LOG_ONCE("gpu: a draw reads %llu bytes of vertices; it is skipped", (unsigned long long)bytes);
                    return false;
                }
                // Past the end of the buffer the vertices read as zero.
                op.vertices.resize(at + size_t(bytes));
                size_t copied = begin < source.bytes ? size_t(std::min<uint64_t>(bytes, source.bytes - begin)) : 0;
                if (copied != 0)
                    memcpy(op.vertices.data() + at, mem::Ptr(source.address + uint32_t(begin)), copied);
                memset(op.vertices.data() + at + copied, 0, size_t(bytes) - copied);
            }
            return true;
        }

        // Builds the host draw from the state the commit left. inlineStream receives the
        // space of the BeginVertices stream in the vertex data. Null when the draw is skipped.
        std::unique_ptr<DrawOp> Capture(PPCContext& ctx, uint8_t* base, const DrawCall& call,
            std::optional<InlineStream>* inlineStream = nullptr)
        {
            Device& device = GetDevice();
            uint32_t devAddress = device.Address();
            Renderer* renderer = ActiveRenderer();
            if (devAddress == 0 || renderer == nullptr || call.count == 0)
                return nullptr;

            uint32_t vertexObject = mem::Load32(devAddress + dev::kVertexShader);
            uint32_t pixelObject = mem::Load32(devAddress + dev::kPixelShader);
            SyncShaders();
            const ShaderInfo* vertexShader = FindShader(vertexObject);
            if (vertexShader == nullptr || vertexShader->pixel)
            {
                LogUnregistered(vertexObject, false);
                return nullptr;
            }
            const ShaderInfo* pixelShader = nullptr;
            if (pixelObject != 0)
            {
                pixelShader = FindShader(pixelObject);
                if (pixelShader == nullptr || !pixelShader->pixel)
                {
                    LogUnregistered(pixelObject, true);
                    return nullptr;
                }
            }
            bool depthOnly = (device.Register(reg::kEdramMode) & kEdramModeMask) == kEdramDepthOnly;
            if (depthOnly != (pixelObject == 0))
            {
                LOG_ONCE("gpu: depth-only mode %u with pixel shader %08X is not covered; draws with it are skipped",
                    device.Register(reg::kEdramMode) & kEdramModeMask, pixelObject);
                return nullptr;
            }
            if (depthOnly && (mem::Load32(vertexObject + shader::kVertexContainer) & shader::kFlagDepthOnlyVariant) != 0)
            {
                LOG_ONCE("gpu: the depth-only variant of a vertex shader is not covered; draws with it are skipped");
                return nullptr;
            }

            std::array<uint8_t, 16> strides;
            memcpy(strides.data(), mem::Ptr(devAddress + dev::kStreamStrides), strides.size());
            if (call.inlineVertices != 0)
                strides[0] = kInlineStride / 4;
            const VertexLayout* layout = FindVertexLayout(ctx, base, vertexObject, *vertexShader,
                mem::Load32(devAddress + dev::kVertexDeclaration), strides);
            if (layout == nullptr)
                return nullptr;

            std::unique_ptr<DrawOp> op = renderer->NewDraw();
            op->vertexShader = vertexShader->key;
            op->pixelShader = pixelShader != nullptr ? pixelShader->key : 0;
            op->input = layout->input;
            if (!ReadState(device, devAddress, *op) || !ReadTargets(device, *op, depthOnly))
                return nullptr;

            // Vertex streams: where each comes from, and how the shader decodes its elements.
            const VertexInput& input = *layout->input;
            size_t streams = layout->streamFetch.size();
            if (streams > kMaxVertexStreams)
            {
                LOG_ONCE("gpu: a draw reads %zu vertex streams; it is skipped", streams);
                return nullptr;
            }
            std::array<StreamPlan, kMaxVertexStreams> plans;
            bool allHosted = true;
            for (size_t stream = 0; stream < streams; stream++)
            {
                uint32_t fetch = layout->streamFetch[stream];
                StreamPlan& plan = plans[stream];
                plan.inlineFetch = call.inlineVertices != 0 && fetch == kInlineFetch;
                if (plan.inlineFetch)
                {
                    plan.source = { call.inlineVertices, kInlineVertices * kInlineStride, kEndian8in32 };
                }
                else if (!ReadStreamSource(devAddress, fetch, plan.source))
                {
                    LOG_ONCE("gpu: vertex fetch constant %u is not a vertex buffer; its draws are skipped", fetch);
                    return nullptr;
                }
                // A stream of stride 0 holds one vertex and is always copied.
                plan.hosted = !plan.inlineFetch && input.strides[stream] != 0 &&
                    UseHostedBuffer({ GpuAddress(plan.source.address), plan.source.bytes });
                allHosted &= plan.hosted;
                for (size_t element = 0; element < layout->descriptors.size(); element++)
                    if (input.elements[element].stream == stream)
                        op->shared.vertexFetch[element] =
                            layout->descriptors[element] | plan.source.endian << kDescriptorEndianShift;
            }

            // A triangle list over buffers the host has copies of is drawn from them as it is. A negative
            // base vertex can read before the vertex buffers, which the copy path checks.
            std::optional<BufferKey> indexBuffer;
            if (allHosted && call.indexed && call.primitive == kTriangleList && call.baseVertex >= 0)
                indexBuffer = HostedIndexBuffer(devAddress, call);
            if (indexBuffer)
            {
                op->hostedIndices = { *indexBuffer, call.startIndex, call.count - call.count % 3, call.baseVertex };
                if (op->hostedIndices.count == 0)
                    return nullptr;
                for (size_t stream = 0; stream < streams; stream++)
                {
                    const StreamSource& source = plans[stream].source;
                    op->hosted[stream] = { GpuAddress(source.address), source.bytes, 0, source.bytes };
                }
            }
            else if (!CaptureCopied(device, devAddress, call, *layout, plans, *op, inlineStream))
            {
                return nullptr;
            }

            // Constants, as the host shaders read them.
            std::optional<ConstantRange> vertexConstants = g_vertexConstants.Store(*renderer, vertexShader->constantRegisters);
            std::optional<ConstantRange> pixelConstants =
                g_pixelConstants.Store(*renderer, pixelShader != nullptr ? pixelShader->constantRegisters : 0);
            if (!vertexConstants || !pixelConstants)
                return nullptr;
            op->vertexConstants = *vertexConstants;
            op->pixelConstants = *pixelConstants;
            op->shared.booleans = (mem::Load32(devAddress + dev::kVertexBooleans) & 0xFFFF) |
                (mem::Load32(devAddress + dev::kPixelBooleans) & 0xFFFF) << 16;
            // Last: the uploads it posts are only needed by a draw that is posted.
            ReadDrawTextures(devAddress, *op);
            return op;
        }

        void Post(std::unique_ptr<DrawOp> op)
        {
            if (!op)
                return;
            if (Renderer* renderer = ActiveRenderer())
                renderer->Draw(std::move(op));
        }

        // Posts the quad of the last BeginVertices, if there is one: before anything else its own
        // thread posts (anyThread false), or before any draw (true). A draw means the drawing thread
        // holds the device, so a quad another thread began has its vertices by then; and the draws
        // then reach the owner in the order they were captured, which their constant ranges require
        // (Renderer::ReserveConstants).
        void PostInlineDraw(bool anyThread)
        {
            std::lock_guard lock(g_pendingMutex);
            if (!g_pending.op || (!anyThread && g_pending.thread != std::this_thread::get_id()))
                return;
            std::unique_ptr<DrawOp> op = std::move(g_pending.op);
            if (g_pending.stream)
                memcpy(op->vertices.data() + g_pending.stream->offset, mem::Ptr(g_pending.vertices), g_pending.stream->bytes);
            Post(std::move(op));
        }
    }

    void FlushInlineDraw()
    {
        PostInlineDraw(false);
    }
}

using namespace gpu::guest;

DECLARE_ORIGINAL(D3D_BEGIN_VERTICES);

// D3DDevice_DrawIndexedVertices(device, primitive, base vertex, start index, index count): commits
// the state and writes the draw packet. Replaced to commit without the packets, which nothing
// executes, and to post the host draw; the constant shadows follow the dirty masks before the
// commit clears them.
PPC_FUNC(D3D_DRAW_INDEXED_VERTICES)
{
    DrawCall call;
    call.primitive = ctx.r4.u32;
    call.indexed = true;
    call.baseVertex = int32_t(ctx.r5.u32);
    call.startIndex = ctx.r6.u32;
    call.count = ctx.r7.u32;
    PostInlineDraw(true);
    RefreshConstants();
    Commit(ctx, base, ctx.r3.u32);
    Post(Capture(ctx, base, call));
}

// D3DDevice_DrawVertices(device, primitive, start vertex, vertex count): commits the state and
// writes the draw packet. Replaced as DrawIndexedVertices is.
PPC_FUNC(D3D_DRAW_VERTICES)
{
    DrawCall call;
    call.primitive = ctx.r4.u32;
    call.baseVertex = int32_t(ctx.r5.u32);
    call.count = ctx.r6.u32;
    PostInlineDraw(true);
    RefreshConstants();
    Commit(ctx, base, ctx.r3.u32);
    Post(Capture(ctx, base, call));
}

// D3DDevice_BeginVertices(device, ...): commits the state and returns the space for the vertices
// of a quad (null on failure). Replaced to build the host draw of the quad after the original,
// whose packets nothing executes (constants as in DrawIndexedVertices); the title writes the
// vertices after the call and ends with an inline EndVertices, so the quad is kept until the next
// draw or until its thread posts something else (FlushInlineDraw).
PPC_FUNC(D3D_BEGIN_VERTICES)
{
    PostInlineDraw(true);
    RefreshConstants();
    ORIGINAL(D3D_BEGIN_VERTICES)(ctx, base);
    uint32_t vertices = ctx.r3.u32;
    if (vertices == 0)
        return;
    DrawCall call;
    call.primitive = kTriangleFan;
    call.count = kInlineVertices;
    call.inlineVertices = vertices;
    std::optional<InlineStream> stream;
    std::unique_ptr<gpu::DrawOp> op = Capture(ctx, base, call, &stream);
    if (!op)
        return;
    std::lock_guard lock(g_pendingMutex);
    g_pending = { std::move(op), vertices, stream, std::this_thread::get_id() };
}
