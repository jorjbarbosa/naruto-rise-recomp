#pragma once

#include "gpu/renderer/renderer.h"

#include <plume_render_interface.h>

#include <cstdint>
#include <memory>
#include <unordered_map>

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
        std::unique_ptr<plume::RenderTexture> texture;
        std::unique_ptr<plume::RenderFramebuffer> framebuffer; // for the resolve passes
        std::unique_ptr<plume::RenderDescriptorSet> sampling;  // for the present
    };

    // The host side of the guest surfaces and of the texture memory they are resolved into.
    // Owner thread only.
    class Targets
    {
    public:
        Targets(Context& context, const Passes& passes) : context_(context), passes_(passes) {}

        void Clear(const ClearOp& op);
        void Resolve(const ResolveOp& op);
        void ReleaseSurface(uint32_t key);
        void ReleaseTexture(uint32_t address, uint32_t header);

        // The texture resolved at a GPU address, or null when no resolve has written there.
        ResolvedTexture* Resolved(uint32_t address);

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
        ResolvedTexture& EnsureResolved(uint32_t address, uint32_t width, uint32_t height, bool depth);
        void ResolvePass(Surface& source, ResolvedTexture& dest, const Rect& region, uint32_t destX, uint32_t destY,
            int32_t sample);

        Context& context_;
        const Passes& passes_;
        std::unordered_map<uint32_t, Surface> surfaces_;
        std::unordered_map<uint32_t, ResolvedTexture> resolved_;
    };
}
