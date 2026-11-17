#pragma once

#include "gpu/renderer/descriptors.h"
#include "gpu/renderer/renderer.h"

#include <plume_render_interface.h>

#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

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
        // The bounds of what the resolves wrote since the texture last became the latest of its
        // address, when it was cleared: outside them it reads zero.
        Rect region;
        uint32_t header = 0; // the guest texture header of the last resolve into it
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
        static constexpr uint32_t kColorBytes = 4;
        static constexpr plume::RenderFormat kDepthFormat = plume::RenderFormat::D32_FLOAT_S8_UINT;

        Targets(Context& context, const Passes& passes, Descriptors& descriptors)
            : context_(context), passes_(passes), descriptors_(descriptors) {}

        void Clear(const ClearOp& op);
        void Resolve(const ResolveOp& op);
        void ReleaseSurface(uint32_t key);
        // Releases the textures resolved at a GPU address whose last resolve came through the header.
        void ReleaseTexture(uint32_t address, uint32_t header);
        // Releases the textures resolved at the GPU addresses [begin, end), whatever header made them.
        void ReleaseResolved(uint32_t begin, uint32_t end);

        // The texture the latest resolve to a GPU address wrote, or null when there is none (no resolve
        // yet, or that texture was released). Valid until the next resolve or release.
        ResolvedTexture* Resolved(uint32_t address);
        // A number that moves whenever Resolved may answer differently, or a resolved texture was left
        // in a layout other than the one its readers put it in: a resolve, a readback, a release.
        uint64_t ResolvedChanges() const { return resolvedChanges_; }
        // Copies the color texture resolved at a GPU address back from the GPU, waiting for the queue
        // to go idle. False when there is none (depth resolves included).
        bool ReadResolved(uint32_t address, ResolvedImage& image);

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

        // The textures resolved at one GPU address, one per (size, kind) resolved there. The title
        // resolves surfaces of several sizes into one piece of memory within a frame (the scene, then
        // the 256x256 and 128x128 shadow maps): each keeps its texture, so a steady frame creates none.
        // A texture goes when the header of its last resolve is destroyed or the memory is freed; a
        // header has one size, so an address holds at most a color and a depth texture per live header.
        struct ResolvedAddress
        {
            std::vector<std::unique_ptr<ResolvedTexture>> textures;
            ResolvedTexture* latest = nullptr; // the one the last resolve wrote; null: it was released
        };

        // The surface of a description, (re)created when the guest describes it differently.
        Surface& EnsureSurface(const SurfaceDesc& desc);
        void RetireSurface(uint32_t key);
        // The texture of (address, size, kind), created on first use, as the latest of its address.
        // One that was not the latest is cleared (recorded in the open command list).
        ResolvedTexture& EnsureResolved(uint32_t address, uint32_t width, uint32_t height, bool depth);
        void ResolvePass(Surface& source, ResolvedTexture& dest, const Rect& region, uint32_t destX, uint32_t destY,
            int32_t sample);

        Context& context_;
        const Passes& passes_;
        Descriptors& descriptors_;
        std::unordered_map<uint32_t, Surface> surfaces_;
        std::unordered_map<uint32_t, ResolvedAddress> resolved_;
        uint64_t resolvedChanges_ = 0;
        // The framebuffers of the draws, by (color key, depth key).
        std::map<std::pair<uint32_t, uint32_t>, std::unique_ptr<plume::RenderFramebuffer>> drawFramebuffers_;
    };
}
