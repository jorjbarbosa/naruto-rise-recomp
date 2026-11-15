#pragma once

#include "gpu/renderer/descriptors.h"
#include "gpu/renderer/renderer.h"

#include <plume_render_interface.h>

#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>

namespace gpu
{
    class Context;
    class Passes;

    // A texture holding resolved guest memory: RGBA8 from color surfaces, R32_FLOAT from depth.
    struct ResolvedTexture
    {
        uint32_t width = 0;
        uint32_t height = 0;
        bool depth = false;
        bool written = false; // a resolve has written it
        uint32_t header = 0;  // the guest texture header of the last resolve into it
        plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
        std::unique_ptr<plume::RenderTexture> texture;
        std::unique_ptr<plume::RenderFramebuffer> framebuffer; // for the resolve passes
        std::unique_ptr<plume::RenderDescriptorSet> sampling;  // for the present
        TextureDescriptor descriptor;                          // for the draws that sample it
        TextureViews views; // for the draws that read it through another format's swizzle
    };

    // The host side of the guest surfaces and of the texture memory they are resolved into.
    // Owner thread only.
    class Targets
    {
    public:
        // Host formats of the guest surfaces.
        static constexpr plume::RenderFormat kColorFormat = plume::RenderFormat::R8G8B8A8_UNORM;
        static constexpr plume::RenderFormat kDepthFormat = plume::RenderFormat::D32_FLOAT_S8_UINT;

        Targets(Context& context, const Passes& passes, Descriptors& descriptors)
            : context_(context), passes_(passes), descriptors_(descriptors) {}

        void Clear(const ClearOp& op);
        void Resolve(const ResolveOp& op);
        void ReleaseSurface(uint32_t key);
        void ReleaseTexture(uint32_t address, uint32_t header);
        // Releases the textures resolved at the GPU addresses [begin, end), whatever header made them.
        void ReleaseResolved(uint32_t begin, uint32_t end);

        // The texture resolved at a GPU address, or null when no resolve has written there.
        ResolvedTexture* Resolved(uint32_t address);

        // Binds the targets of a draw (either may have key 0), recording the barriers that put
        // them in the layouts it writes.
        void BindForDraw(plume::RenderCommandList& list, const SurfaceDesc& color, const SurfaceDesc& depth);

    private:
        struct Surface
        {
            SurfaceDesc desc;
            std::unique_ptr<plume::RenderTexture> texture;
            std::unique_ptr<plume::RenderTextureView> view; // the color, or the depth plane
            std::unique_ptr<plume::RenderFramebuffer> framebuffer;
            std::unique_ptr<plume::RenderDescriptorSet> sampling; // created by the first resolve pass
        };

        // The surface of a description, (re)created when the guest describes it differently.
        Surface& EnsureSurface(const SurfaceDesc& desc);
        void RetireSurface(uint32_t key);
        ResolvedTexture& EnsureResolved(uint32_t address, uint32_t width, uint32_t height, bool depth);
        void ResolvePass(Surface& source, ResolvedTexture& dest, const Rect& region, uint32_t destX, uint32_t destY,
            int32_t sample);

        Context& context_;
        const Passes& passes_;
        Descriptors& descriptors_;
        std::unordered_map<uint32_t, Surface> surfaces_;
        std::unordered_map<uint32_t, ResolvedTexture> resolved_;
        // The framebuffers of the draws, by (color key, depth key).
        std::map<std::pair<uint32_t, uint32_t>, std::unique_ptr<plume::RenderFramebuffer>> drawFramebuffers_;
    };
}
