#pragma once

#include "gpu/textures/layout.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace gpu
{
    struct Settings;

    // A guest render target or depth-stencil surface. The host keeps one persistent texture per
    // key (RGBA8 or D32S8, with the guest's sample count); there is no EDRAM.
    struct SurfaceDesc
    {
        uint32_t key = 0; // guest surface header; 0: no surface
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t samples = 1;
        bool depth = false;

        bool operator==(const SurfaceDesc&) const = default;
    };

    struct Rect
    {
        int32_t left = 0;
        int32_t top = 0;
        int32_t right = 0;
        int32_t bottom = 0;

        bool Empty() const { return left >= right || top >= bottom; }
    };

    inline Rect Intersect(const Rect& a, const Rect& b)
    {
        return { std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right),
            std::min(a.bottom, b.bottom) };
    }

    struct ClearOp
    {
        std::array<SurfaceDesc, 4> colors; // the render targets to clear (key 0: none)
        SurfaceDesc depth;                 // key 0: no depth-stencil clear
        bool clearDepth = false;
        bool clearStencil = false;
        std::array<float, 4> color{};
        float depthValue = 0.0f;
        uint8_t stencil = 0;
        std::optional<Rect> rect; // none: the whole surfaces
    };

    // A copy of a surface region into texture memory (D3DDevice_Resolve). The destination is
    // identified by its GPU address: a texture there is the content of that guest memory.
    struct ResolveOp
    {
        SurfaceDesc source;
        int32_t sample = -1; // the sample copied; -1: all samples, averaged (depth: sample 0)
        Rect sourceRect;
        uint32_t destX = 0;
        uint32_t destY = 0;
        uint32_t destAddress = 0;
        uint32_t destWidth = 0;
        uint32_t destHeight = 0;
        uint32_t destHeader = 0; // the guest texture header resolved into
    };

    // The shared constant buffer of the translated shaders (register b2, space4), as laid out by
    // tools/XenosRecomp/XenosRecomp/shader_common.h with patches/XenosRecomp/0003, 0005 and 0006.
    struct SharedConstants
    {
        static constexpr uint32_t kFetchSlots = 32;
        static constexpr uint32_t kVertexElements = 16;
        static constexpr uint32_t kClipPlaneCount = 6;

        // Descriptor indices by texture fetch constant: 2D, 3D and cube textures, samplers.
        uint32_t texture2D[kFetchSlots] = {};
        uint32_t texture3D[kFetchSlots] = {};
        uint32_t textureCube[kFetchSlots] = {};
        uint32_t sampler[kFetchSlots] = {};
        uint32_t booleans = 0;      // vertex shader b0..b15, then pixel shader b0..b15
        uint32_t specConstants = 0; // SPEC_CONSTANT_* bits
        float halfPixelOffset[2] = {};
        float alphaThreshold = 0.0f;
        uint32_t reserved[3] = {};
        uint32_t vertexFetch[kVertexElements] = {}; // fetch descriptor of each vertex element (vfetch)
        // User clip planes in clip space (x, y, z, w; a point is kept where the dot product is
        // not negative); a disabled plane is zero and clips nothing.
        float clipPlanes[kClipPlaneCount][4] = {};
    };
    static_assert(sizeof(SharedConstants) == 44 * 16);

    // SPEC_CONSTANT_ALPHA_TEST: the pixel shader discards pixels whose alpha is below the threshold.
    constexpr uint32_t kSpecAlphaTest = 2;

    // A vertex element of a draw: the raw dwords of one vertex shader input, at an offset in a
    // vertex stream. The shader decodes them (SharedConstants::vertexFetch).
    struct VertexElement
    {
        uint8_t usage = 0; // D3DDECLUSAGE: the input semantic
        uint8_t usageIndex = 0;
        uint8_t stream = 0;
        uint8_t dwords = 0; // 1 to 4
        uint32_t offset = 0;

        bool operator==(const VertexElement&) const = default;
    };

    // The vertex input of a draw. Shared by the draws of one (vertex shader, declaration, strides).
    struct VertexInput
    {
        std::vector<VertexElement> elements; // in the order of the shader's inputs
        std::vector<uint32_t> strides;       // bytes, per stream
        uint64_t id = 0;                     // hash of the elements and strides, for the pipeline key
    };

    // The fixed-function state of a draw: guest register values, with the fields that do not apply
    // cleared so that equal pipelines have equal bytes. Hashed and compared as bytes.
    struct RenderState
    {
        uint32_t depthControl = 0;   // RB_DEPTHCONTROL
        uint32_t stencilMasks = 0;   // RB_STENCILREFMASK without the reference: read mask, write mask
        uint32_t blendControl = 0;   // RB_BLENDCONTROL0
        uint32_t cullControl = 0;    // PA_SU_SC_MODE_CNTL bits 0..2: cull front, cull back, clockwise front
        uint32_t colorMask = 0;      // RB_COLOR_MASK bits 0..3 (render target 0)
        uint32_t alphaToCoverage = 0;
        // Polygon offset as the host depth bias: the constant in the integer unit of a 24-bit
        // depth buffer (which the host's float buffer scales by the depth's exponent), the slope
        // per pixel; 0 without depth.
        int32_t depthBias = 0;
        float depthBiasSlope = 0;

        bool operator==(const RenderState&) const = default;
    };

    // The sampler state of a texture fetch constant. The levels sampled are part of the texture's
    // view (DrawTexture::mipMin, mipMax), so few samplers cover every texture.
    struct SamplerState
    {
        uint32_t clamp = 0;  // fetch dword 0 bits 10..18: the clamp modes of X, Y and Z
        uint32_t filter = 0; // fetch dword 3 bits 19..27: mag, min, mip and anisotropic filters
        int32_t lodBias = 0; // in 1/32
        uint32_t border = 0; // fetch dword 5 bits 0..1
    };

    // How a draw samples the texture of a fetch constant.
    struct DrawTexture
    {
        enum class Kind : uint8_t
        {
            None,   // no texture: the slot reads as zero
            Planar, // a 1D or 2D texture (the 2D set)
            Cube,
        };

        Kind kind = Kind::None;
        // The texture resolved at the address, or the one uploaded for (address, layout).
        bool resolved = false;
        uint32_t address = 0;
        uint64_t layout = 0;
        uint32_t format = 0;  // GPUTEXTUREFORMAT
        uint32_t swizzle = 0; // fetch swizzle: 3 bits per component (x, y, z, w, 0, 1)
        uint32_t mipMin = 0;  // the levels sampled (uploaded textures)
        uint32_t mipMax = 0;
        SamplerState sampler;
    };

    // The content of a guest texture, copied from guest memory for the host texture of (address,
    // layout key): the ranges of memory its layout reads, back to back in data.
    struct TextureUpload
    {
        uint32_t address = 0;
        uint64_t layoutKey = 0;
        textures::Layout layout;
        std::vector<std::pair<uint32_t, uint32_t>> ranges; // GPU address, bytes
        std::vector<uint8_t> data;
    };

    // The content of a guest vertex buffer for the host copy of (GPU address, data size), as it is in
    // guest memory.
    struct BufferUpload
    {
        uint32_t address = 0;
        std::vector<uint8_t> data;
    };

    // A vertex stream a draw reads from the host copy of a guest vertex buffer: the buffer's key, and
    // the bytes of the vertices the draw reads in it.
    struct HostedStream
    {
        uint32_t address = 0; // GPU address of the buffer
        uint32_t bytes = 0;   // the buffer's size; 0: the stream is in DrawOp::vertices
        uint32_t offset = 0;  // the first vertex the draw reads
        uint32_t size = 0;
    };

    // A color texture resolved from a surface, read back: RGBA8 in the surface's color order, rows
    // without padding. Only the region the resolves wrote holds their colors.
    struct ResolvedImage
    {
        uint32_t width = 0;
        uint32_t height = 0;
        Rect region;
        std::vector<uint8_t> rgba;
    };

    // The title's gamma ramp (D3DGAMMARAMP): 256 red, then 256 green, then 256 blue entries, each a
    // 10-bit value in the high bits of a 16-bit word.
    using GammaRamp = std::array<uint16_t, 3 * 256>;
    constexpr uint32_t kGammaRampShift = 6;

    // An allocator whose resize leaves the new elements uninitialized, for the buffers of a draw that
    // the capture fills at once (zeroing them first doubled the writes of every draw).
    template <typename T>
    struct UninitializedAllocator : std::allocator<T>
    {
        template <typename U>
        struct rebind
        {
            using other = UninitializedAllocator<U>;
        };

        UninitializedAllocator() = default;
        template <typename U>
        UninitializedAllocator(const UninitializedAllocator<U>&) noexcept
        {
        }

        template <typename U>
        void construct(U* p) noexcept
        {
            ::new (static_cast<void*>(p)) U;
        }
        template <typename U, typename... Args>
        void construct(U* p, Args&&... args)
        {
            ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
        }
    };
    template <typename T>
    using CaptureVector = std::vector<T, UninitializedAllocator<T>>;

    // A draw: shaders, targets, state, and copies of the vertices, indices and constants it reads.
    // Everything is a triangle list by then. Made with std::make_unique_for_overwrite: the capture
    // writes every byte of the constants, which value initialization would zero first.
    struct DrawOp
    {
        uint64_t vertexShader = 0; // shader cache keys; pixel shader 0: depth only
        uint64_t pixelShader = 0;
        SurfaceDesc color;         // key 0: no color target
        SurfaceDesc depth;         // key 0: no depth-stencil target
        RenderState state;
        uint8_t stencilReference = 0; // per draw, not part of the pipeline
        std::array<float, 4> blendColor{};
        std::shared_ptr<const VertexInput> input;

        // Vertex data: stream i starts at streamOffsets[i] and is laid out with its stride, unless
        // hosted[i] names the host copy it reads instead.
        CaptureVector<uint8_t> vertices;
        std::vector<uint32_t> streamOffsets;
        std::vector<HostedStream> hosted;
        CaptureVector<uint32_t> indices;
        uint32_t vertexCount = 0;

        std::array<uint8_t, 4096> vertexConstants;
        std::array<uint8_t, 4096> pixelConstants;
        SharedConstants shared; // descriptor indices filled by the renderer from textures
        std::array<DrawTexture, SharedConstants::kFetchSlots> textures{}; // by texture fetch constant

        float viewport[6] = {}; // x, y, width, height, min depth, max depth
        Rect scissor;
    };

    // The console's vertical blank rate: presentation intervals count its vblanks.
    constexpr uint32_t kVblankRate = 60;

    // The host GPU: a D3D12 device, queue and swap chain through Plume. Operations are posted from
    // the guest threads and run in posting order on the owner thread, which alone touches the
    // D3D12 objects. A completion thread waits for the GPU and reports the end of each batch.
    class Renderer
    {
    public:
        // Called on the completion thread with the token of each Kick, in posting order.
        using Completion = std::function<void(uint32_t token)>;
        // Called on the posting thread before each operation is queued, so that the poster can queue
        // work that must come first. It may post itself.
        using BeforePost = std::function<void()>;

        // Null when the device or the swap chain cannot be created (logged).
        static std::unique_ptr<Renderer> Create(void* window, const Settings& settings, Completion completion,
            BeforePost beforePost);
        ~Renderer();

        // Ends a batch: completion(token) runs once the GPU has finished everything posted before.
        void Kick(uint32_t token);

        void Clear(const ClearOp& op);
        void Resolve(const ResolveOp& op);
        void Draw(std::unique_ptr<DrawOp> op);
        // Fills the host texture of (address, layout key) with new content; draws posted after it
        // sample it.
        void UploadTexture(std::unique_ptr<TextureUpload> upload);
        // Fills the host copy of the vertex buffer (address, data size); draws posted after it read it.
        void UploadBuffer(std::unique_ptr<BufferUpload> upload);

        // The guest surface is gone; its host texture is released once the GPU no longer uses it.
        void ReleaseSurface(uint32_t key);
        // A guest texture header is gone. The textures uploaded from its address are released, and
        // the texture resolved there if that header made it (other headers over the same memory
        // leave a resolved texture alone).
        void ReleaseTexture(uint32_t address, uint32_t header);
        // Guest memory at the GPU addresses [begin, end) is freed: every texture uploaded or resolved
        // there is released.
        void ReleaseMemory(uint32_t begin, uint32_t end);
        // The host copies of the vertex buffers that overlap the GPU addresses [begin, end) go.
        void ReleaseBuffers(uint32_t begin, uint32_t end);
        // The host copy of the vertex buffer (address, bytes) goes.
        void ReleaseBuffer(uint32_t address, uint32_t bytes);

        // Reads back the color texture resolved at a GPU address, once the work posted before it has
        // run. Blocks the caller until the GPU is idle; for the rare CPU reads of a resolve. False when
        // nothing (or a depth surface) was resolved there.
        bool ReadResolved(uint32_t address, ResolvedImage& image);

        // Presents the texture resolved at the given GPU address, fitted to the window (black when
        // nothing was resolved there). When the display can pace (vsync on, a window with an area,
        // refreshMillihertz within 1 Hz of a multiple of 60 Hz) and vblanks != 0, the image is held
        // for vblanks vertical blanks of 60 Hz after the previous one; otherwise it is presented at
        // once. Returns the number to pass to WaitPresented, 0 when the owner is stopping.
        uint64_t Present(uint32_t frontAddress, uint32_t vblanks, uint32_t refreshMillihertz);
        // Blocks until the present is handed to the swap chain, which for a paced present is once the
        // previous image is on screen. True when the display paced it. Any thread.
        bool WaitPresented(uint64_t present);
        // Whether the display paced the last present: a hint for the caller to wait for the next one
        // instead of pacing itself. Any thread.
        bool DisplayPaces() const;
        // The ramp the presents posted after it map the front buffer through.
        void SetGammaRamp(const GammaRamp& ramp);

        class Impl;

    private:
        explicit Renderer(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
    };
}
