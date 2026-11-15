#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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
    // tools/XenosRecomp/XenosRecomp/shader_common.h with patches/XenosRecomp/0003 and 0005.
    struct SharedConstants
    {
        static constexpr uint32_t kFetchSlots = 32;
        static constexpr uint32_t kVertexElements = 16;

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
    };
    static_assert(sizeof(SharedConstants) == 38 * 16);

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
        uint32_t stencilRefMask = 0; // RB_STENCILREFMASK: reference, read mask, write mask
        uint32_t blendControl = 0;   // RB_BLENDCONTROL0
        uint32_t cullControl = 0;    // PA_SU_SC_MODE_CNTL bits 0..2: cull front, cull back, clockwise front
        uint32_t colorMask = 0;      // RB_COLOR_MASK bits 0..3 (render target 0)
        uint32_t alphaToCoverage = 0;

        bool operator==(const RenderState&) const = default;
    };

    // A draw: shaders, targets, state, and copies of the vertices, indices and constants it reads.
    // Everything is a triangle list by then.
    struct DrawOp
    {
        uint64_t vertexShader = 0; // shader cache keys; pixel shader 0: depth only
        uint64_t pixelShader = 0;
        SurfaceDesc color;         // key 0: no color target
        SurfaceDesc depth;         // key 0: no depth-stencil target
        RenderState state;
        std::array<float, 4> blendColor{};
        std::shared_ptr<const VertexInput> input;

        // Vertex data: stream i starts at streamOffsets[i] and is laid out with its stride.
        std::vector<uint8_t> vertices;
        std::vector<uint32_t> streamOffsets;
        std::vector<uint32_t> indices;
        uint32_t vertexCount = 0;

        std::array<uint8_t, 4096> vertexConstants{};
        std::array<uint8_t, 4096> pixelConstants{};
        SharedConstants shared; // descriptor indices filled by the renderer from textures
        // Each texture fetch constant: the GPU address of its 2D texture (0: none, or a texture
        // that is not 2D) and the dwords with its sampler state (0 and 3).
        struct Texture
        {
            uint32_t address = 0;
            uint32_t sampler0 = 0;
            uint32_t sampler3 = 0;
        };
        std::array<Texture, SharedConstants::kFetchSlots> textures{};

        float viewport[6] = {}; // x, y, width, height, min depth, max depth
        Rect scissor;
    };

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

        // The guest surface is gone; its host texture is released once the GPU no longer uses it.
        void ReleaseSurface(uint32_t key);
        // A guest texture header is gone. The texture resolved at its address is released if that
        // header made it; other headers over the same memory leave it alone.
        void ReleaseTexture(uint32_t address, uint32_t header);

        // Presents the texture resolved at the given GPU address, fitted to the window (black when
        // nothing was resolved there).
        void Present(uint32_t frontAddress);

        class Impl;

    private:
        explicit Renderer(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
    };
}
