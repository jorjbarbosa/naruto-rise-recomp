// Guest draws: translation of the guest state into pipelines and recording of the draws.

#include "gpu/renderer/draws.h"

#include "gpu/renderer/context.h"
#include "gpu/renderer/descriptors.h"
#include "gpu/renderer/targets.h"
#include "gpu/renderer/textures.h"
#include "log.h"

#include <plume_d3d12.h>

#include <algorithm>
#include <cstring>

namespace gpu
{
    namespace
    {
        // Root constant buffers of the translated shaders (register b0..b2, space 4).
        constexpr uint32_t kConstantSpace = 4;
        constexpr uint32_t kVertexConstantsRoot = 0;
        constexpr uint32_t kPixelConstantsRoot = 1;
        constexpr uint32_t kSharedConstantsRoot = 2;
        constexpr uint64_t kConstantAlignment = 256;

        // D3DDECLUSAGE, as the translated shaders name their inputs (the guest drops other usages).
        const char* const kSemantics[] = { "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD",
            "TANGENT", "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE" };

        constexpr plume::RenderFormat kElementFormats[] = { plume::RenderFormat::R32_UINT, plume::RenderFormat::R32G32_UINT,
            plume::RenderFormat::R32G32B32_UINT, plume::RenderFormat::R32G32B32A32_UINT };

        // RB_DEPTHCONTROL fields.
        constexpr uint32_t kStencilEnable = 0x1;
        constexpr uint32_t kDepthEnable = 0x2;
        constexpr uint32_t kDepthWrite = 0x4;
        constexpr uint32_t kBackFaceStencil = 0x80;
        // PA_SU_SC_MODE_CNTL.
        constexpr uint32_t kCullFront = 0x1;
        constexpr uint32_t kCullBack = 0x2;
        constexpr uint32_t kFrontClockwise = 0x4;

        // GPU compare functions (NEVER, LESS, EQUAL, LEQUAL, GREATER, NOTEQUAL, GEQUAL, ALWAYS)
        // and stencil operations (KEEP, ZERO, REPLACE, INCR_SAT, DECR_SAT, INVERT, INCR, DECR) are
        // in the order of Plume's, after its UNKNOWN.
        plume::RenderComparisonFunction Compare(uint32_t function)
        {
            return plume::RenderComparisonFunction(function + 1);
        }

        plume::RenderStencilFaceDesc StencilFace(uint32_t fields)
        {
            plume::RenderStencilFaceDesc face;
            face.compareFunction = Compare(fields & 7);
            face.failOp = plume::RenderStencilOp(((fields >> 3) & 7) + 1);
            face.passOp = plume::RenderStencilOp(((fields >> 6) & 7) + 1);
            face.depthFailOp = plume::RenderStencilOp(((fields >> 9) & 7) + 1);
            return face;
        }

        // GPU blend factors. The constant alpha factors (14, 15) take the blend color, which the
        // draw sets to its alpha when they are used. In the alpha factors, a color factor stands for
        // its alpha.
        plume::RenderBlend BlendFactor(uint32_t factor, bool alpha)
        {
            switch (factor)
            {
            case 0: return plume::RenderBlend::ZERO;
            case 1: return plume::RenderBlend::ONE;
            case 4: return alpha ? plume::RenderBlend::SRC_ALPHA : plume::RenderBlend::SRC_COLOR;
            case 5: return alpha ? plume::RenderBlend::INV_SRC_ALPHA : plume::RenderBlend::INV_SRC_COLOR;
            case 6: return plume::RenderBlend::SRC_ALPHA;
            case 7: return plume::RenderBlend::INV_SRC_ALPHA;
            case 8: return alpha ? plume::RenderBlend::DEST_ALPHA : plume::RenderBlend::DEST_COLOR;
            case 9: return alpha ? plume::RenderBlend::INV_DEST_ALPHA : plume::RenderBlend::INV_DEST_COLOR;
            case 10: return plume::RenderBlend::DEST_ALPHA;
            case 11: return plume::RenderBlend::INV_DEST_ALPHA;
            case 12: case 14: return plume::RenderBlend::BLEND_FACTOR;
            case 13: case 15: return plume::RenderBlend::INV_BLEND_FACTOR;
            case 16: return plume::RenderBlend::SRC_ALPHA_SAT;
            default:
                LOG_ONCE("gpu: blend factor %u is not covered; it blends as ONE", factor);
                return plume::RenderBlend::ONE;
            }
        }

        plume::RenderBlendOperation BlendOperation(uint32_t operation)
        {
            switch (operation)
            {
            case 0: return plume::RenderBlendOperation::ADD;
            case 1: return plume::RenderBlendOperation::SUBTRACT;
            case 2: return plume::RenderBlendOperation::MIN;
            case 3: return plume::RenderBlendOperation::MAX;
            case 4: return plume::RenderBlendOperation::REV_SUBTRACT;
            default:
                LOG_ONCE("gpu: blend operation %u is not covered; it blends as ADD", operation);
                return plume::RenderBlendOperation::ADD;
            }
        }

        // RB_BLENDCONTROL: color source, operation and destination at bits 0, 5 and 8; alpha at
        // 16, 21 and 24.
        plume::RenderBlendDesc Blend(uint32_t control, uint32_t colorMask)
        {
            plume::RenderBlendDesc blend;
            blend.srcBlend = BlendFactor(control & 0x1F, false);
            blend.blendOp = BlendOperation((control >> 5) & 7);
            blend.dstBlend = BlendFactor((control >> 8) & 0x1F, false);
            blend.srcBlendAlpha = BlendFactor((control >> 16) & 0x1F, true);
            blend.blendOpAlpha = BlendOperation((control >> 21) & 7);
            blend.dstBlendAlpha = BlendFactor((control >> 24) & 0x1F, true);
            // ONE, ADD, ZERO for both: no blending.
            blend.blendEnabled = (control & 0x1FFF1FFF) != 0x00010001;
            blend.renderTargetWriteMask = uint8_t(colorMask);
            return blend;
        }

        // The blend color of a draw: its constant color, or its alpha for the constant alpha factors.
        bool UsesBlendColor(uint32_t control, bool& alphaOnly)
        {
            bool color = false;
            bool alpha = false;
            for (uint32_t factor : { control & 0x1F, (control >> 8) & 0x1F, (control >> 16) & 0x1F, (control >> 24) & 0x1F })
            {
                color |= factor == 12 || factor == 13;
                alpha |= factor == 14 || factor == 15;
            }
            if (color && alpha)
                LOG_ONCE("gpu: a blend uses both the constant color and the constant alpha; the color is used");
            alphaOnly = alpha && !color;
            return color || alpha;
        }
    }

    size_t Draws::PipelineKeyHash::operator()(const PipelineKey& key) const
    {
        // FNV-1a over the bytes: the key has no padding.
        static_assert(sizeof(PipelineKey) == 3 * 8 + sizeof(RenderState) + 2 * 4);
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&key);
        uint64_t hash = 0xCBF29CE484222325ull;
        for (size_t i = 0; i < sizeof(key); i++)
            hash = (hash ^ bytes[i]) * 0x100000001B3ull;
        return size_t(hash);
    }

    Draws::Draws(Context& context, Targets& targets, Descriptors& descriptors, Textures& textures)
        : context_(context), targets_(targets), descriptors_(descriptors), textures_(textures)
    {
        const plume::RenderRootDescriptorDesc roots[] = {
            { kVertexConstantsRoot, kConstantSpace, plume::RenderRootDescriptorType::CONSTANT_BUFFER },
            { kPixelConstantsRoot, kConstantSpace, plume::RenderRootDescriptorType::CONSTANT_BUFFER },
            { kSharedConstantsRoot, kConstantSpace, plume::RenderRootDescriptorType::CONSTANT_BUFFER },
        };
        plume::RenderPipelineLayoutDesc desc(nullptr, 0, descriptors.SetLayouts(), Descriptors::kSetCount, false, true);
        desc.rootDescriptorDescs = roots;
        desc.rootDescriptorDescsCount = uint32_t(std::size(roots));
        layout_ = context.Device().createPipelineLayout(desc);
    }

    Draws::~Draws() = default;

    const plume::RenderShader* Draws::Shader(uint64_t key)
    {
        auto it = shaders_.find(key);
        if (it != shaders_.end())
            return it->second.get();
        std::span<const uint8_t> dxil = cache_.Dxil(key);
        std::unique_ptr<plume::RenderShader> shader;
        if (!dxil.empty())
            shader = context_.Device().createShader(dxil.data(), dxil.size(), "main", plume::RenderShaderFormat::DXIL);
        else
            Log("gpu: shader %016llX is not in the shader cache; its draws are skipped", (unsigned long long)key);
        return shaders_.emplace(key, std::move(shader)).first->second.get();
    }

    const plume::RenderPipeline* Draws::Pipeline(const DrawOp& op)
    {
        PipelineKey key;
        key.vertexShader = op.vertexShader;
        key.pixelShader = op.pixelShader;
        key.input = op.input->id;
        key.state = op.state;
        key.samples = op.color.key != 0 ? op.color.samples : op.depth.samples;
        key.targets = (op.color.key != 0 ? 1 : 0) | (op.depth.key != 0 ? 2 : 0);
        auto it = pipelines_.find(key);
        if (it != pipelines_.end())
            return it->second.get();

        const plume::RenderShader* vertexShader = Shader(op.vertexShader);
        const plume::RenderShader* pixelShader = op.pixelShader != 0 ? Shader(op.pixelShader) : nullptr;
        std::unique_ptr<plume::RenderPipeline> pipeline;
        if (vertexShader != nullptr && (op.pixelShader == 0 || pixelShader != nullptr))
        {
            const VertexInput& input = *op.input;
            std::vector<plume::RenderInputSlot> slots;
            for (uint32_t i = 0; i < input.strides.size(); i++)
                slots.emplace_back(i, input.strides[i]);
            std::vector<plume::RenderInputElement> elements;
            for (uint32_t i = 0; i < input.elements.size(); i++)
            {
                const VertexElement& element = input.elements[i];
                elements.emplace_back(kSemantics[element.usage], element.usageIndex, i, kElementFormats[element.dwords - 1],
                    element.stream, element.offset);
            }

            const RenderState& state = op.state;
            plume::RenderGraphicsPipelineDesc desc;
            desc.pipelineLayout = layout_.get();
            desc.vertexShader = vertexShader;
            desc.pixelShader = pixelShader;
            desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
            desc.inputSlots = slots.data();
            desc.inputSlotsCount = uint32_t(slots.size());
            desc.inputElements = elements.data();
            desc.inputElementsCount = uint32_t(elements.size());
            desc.multisampling.sampleCount = plume::RenderSampleCounts(key.samples);
            desc.alphaToCoverageEnabled = state.alphaToCoverage != 0;
            desc.depthClipEnabled = true;
            if ((state.cullControl & kCullFront) != 0)
                desc.cullMode = plume::RenderCullMode::FRONT;
            else if ((state.cullControl & kCullBack) != 0)
                desc.cullMode = plume::RenderCullMode::BACK;
            else
                desc.cullMode = plume::RenderCullMode::NONE;
            desc.frontFace = (state.cullControl & kFrontClockwise) != 0 ? plume::RenderFrontFace::CLOCKWISE
                                                                          : plume::RenderFrontFace::COUNTER_CLOCKWISE;
            if (op.color.key != 0)
            {
                desc.renderTargetFormat[0] = Targets::kColorFormat;
                desc.renderTargetBlend[0] = Blend(state.blendControl, state.colorMask);
                desc.renderTargetCount = 1;
            }
            if (op.depth.key != 0)
            {
                desc.depthTargetFormat = Targets::kDepthFormat;
                desc.depthEnabled = (state.depthControl & kDepthEnable) != 0;
                desc.depthWriteEnabled = (state.depthControl & kDepthWrite) != 0;
                desc.depthFunction = Compare((state.depthControl >> 4) & 7);
                desc.stencilEnabled = (state.depthControl & kStencilEnable) != 0;
                if (desc.stencilEnabled)
                {
                    desc.stencilReference = state.stencilRefMask & 0xFF;
                    desc.stencilReadMask = (state.stencilRefMask >> 8) & 0xFF;
                    desc.stencilWriteMask = (state.stencilRefMask >> 16) & 0xFF;
                    desc.stencilFrontFace = StencilFace(state.depthControl >> 8);
                    desc.stencilBackFace = (state.depthControl & kBackFaceStencil) != 0
                        ? StencilFace(state.depthControl >> 20)
                        : desc.stencilFrontFace;
                }
            }
            pipeline = context_.Device().createGraphicsPipeline(desc);
            if (static_cast<plume::D3D12GraphicsPipeline*>(pipeline.get())->d3d == nullptr)
            {
                Log("gpu: cannot create the pipeline of shaders %016llX and %016llX; their draws are skipped",
                    (unsigned long long)op.vertexShader, (unsigned long long)op.pixelShader);
                pipeline.reset();
            }
        }
        return pipelines_.emplace(key, std::move(pipeline)).first->second.get();
    }

    void Draws::Draw(DrawOp& op)
    {
        const plume::RenderPipeline* pipeline = Pipeline(op);
        if (pipeline == nullptr)
            return;
        const SurfaceDesc& target = op.color.key != 0 ? op.color : op.depth;
        Rect scissor = Intersect(op.scissor, { 0, 0, int32_t(target.width), int32_t(target.height) });
        if (scissor.Empty())
            return;
        plume::RenderCommandList& list = context_.Commands();

        SharedConstants shared = op.shared;
        for (uint32_t slot = 0; slot < SharedConstants::kFetchSlots; slot++)
            textures_.Bind(list, slot, op.textures[slot], shared);
        targets_.BindForDraw(list, op.color, op.depth);

        // Per-draw data in the upload buffers.
        constexpr uint64_t kSharedBytes = (sizeof(SharedConstants) + kConstantAlignment - 1) & ~(kConstantAlignment - 1);
        Context::Upload constants = context_.Allocate(2 * op.vertexConstants.size() + kSharedBytes, kConstantAlignment);
        memcpy(constants.data, op.vertexConstants.data(), op.vertexConstants.size());
        memcpy(constants.data + op.vertexConstants.size(), op.pixelConstants.data(), op.pixelConstants.size());
        memcpy(constants.data + 2 * op.vertexConstants.size(), &shared, sizeof(shared));
        Context::Upload vertices = context_.Allocate(std::max<size_t>(op.vertices.size(), 4), 16);
        memcpy(vertices.data, op.vertices.data(), op.vertices.size());
        Context::Upload indices = context_.Allocate(op.indices.size() * 4, 4);
        memcpy(indices.data, op.indices.data(), op.indices.size() * 4);

        list.setGraphicsPipelineLayout(layout_.get());
        list.setPipeline(pipeline);
        descriptors_.Bind(list);
        list.setGraphicsRootDescriptor(constants.Reference(), kVertexConstantsRoot);
        list.setGraphicsRootDescriptor({ constants.buffer, constants.offset + op.vertexConstants.size() }, kPixelConstantsRoot);
        list.setGraphicsRootDescriptor({ constants.buffer, constants.offset + 2 * op.vertexConstants.size() },
            kSharedConstantsRoot);

        bool alphaOnly = false;
        if (op.color.key != 0 && UsesBlendColor(op.state.blendControl, alphaOnly))
        {
            float color[4] = { op.blendColor[0], op.blendColor[1], op.blendColor[2], op.blendColor[3] };
            if (alphaOnly)
                std::fill(color, color + 3, op.blendColor[3]);
            static_cast<plume::D3D12CommandList&>(list).d3d->OMSetBlendFactor(color);
        }

        const plume::RenderViewport viewport(op.viewport[0], op.viewport[1], op.viewport[2], op.viewport[3], op.viewport[4],
            op.viewport[5]);
        const plume::RenderRect rect(scissor.left, scissor.top, scissor.right, scissor.bottom);
        list.setViewports(&viewport, 1);
        list.setScissors(&rect, 1);

        const VertexInput& input = *op.input;
        std::vector<plume::RenderVertexBufferView> views;
        std::vector<plume::RenderInputSlot> slots;
        for (uint32_t i = 0; i < input.strides.size(); i++)
        {
            uint32_t end = i + 1 < op.streamOffsets.size() ? op.streamOffsets[i + 1] : uint32_t(op.vertices.size());
            views.emplace_back(plume::RenderBufferReference(vertices.buffer, vertices.offset + op.streamOffsets[i]),
                end - op.streamOffsets[i]);
            slots.emplace_back(i, input.strides[i]);
        }
        list.setVertexBuffers(0, views.data(), uint32_t(views.size()), slots.data());
        const plume::RenderIndexBufferView indexView(indices.Reference(), uint32_t(op.indices.size() * 4),
            plume::RenderFormat::R32_UINT);
        list.setIndexBuffer(&indexView);
        list.drawIndexedInstanced(uint32_t(op.indices.size()), 1, 0, 0, 0);
    }
}
