#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

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

    // The host GPU: a D3D12 device, queue and swap chain through Plume. Operations are posted from
    // the guest threads and run in posting order on the owner thread, which alone touches the
    // D3D12 objects. A completion thread waits for the GPU and reports the end of each batch.
    class Renderer
    {
    public:
        // Called on the completion thread with the token of each Kick, in posting order.
        using Completion = std::function<void(uint32_t token)>;

        // Null when the device or the swap chain cannot be created (logged).
        static std::unique_ptr<Renderer> Create(void* window, const Settings& settings, Completion completion);
        ~Renderer();

        // Ends a batch: completion(token) runs once the GPU has finished everything posted before.
        void Kick(uint32_t token);

        void Clear(const ClearOp& op);
        void Resolve(const ResolveOp& op);

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
