#pragma once

#include "gpu/renderer/renderer.h"

#include <plume_render_interface.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace gpu
{
    class Context;
    class Passes;
    struct ResolvedTexture;
    struct Settings;

    // The swap chain of the game window. Owner thread only.
    class Presenter
    {
    public:
        static constexpr plume::RenderFormat kFormat = plume::RenderFormat::B8G8R8A8_UNORM;

        // Null when the swap chain cannot be created (logged).
        static std::unique_ptr<Presenter> Create(Context& context, void* window, const Settings& settings);

        // The ramp later presents map the image through; the identity until the title sets one.
        void SetGammaRamp(const GammaRamp& ramp);

        // Submits the work recorded so far and presents the image, mapped through the gamma ramp,
        // fitted to the window, centered with black bars; black without an image.
        // When the display can pace (vsync on, a window with an area, refreshMillihertz within 1 Hz of
        // a multiple k of 60 Hz) and vblanks != 0, the presents ask for vblanks x k refreshes since the
        // previous image (split into presents of at most four), so that while the interval stays the
        // same each image stays that long on screen; the present waits until the previous image is on
        // screen first. Otherwise the image is presented at once (at the next vertical blank with
        // vsync). shown(paced) runs once the first present is submitted, or at once when nothing could
        // be presented; paced tells whether the display paced it.
        void Present(const Passes& passes, const ResolvedTexture* image, uint32_t vblanks, uint32_t refreshMillihertz,
            const std::function<void(bool paced)>& shown);

    private:
        explicit Presenter(Context& context, bool vsync);
        void CreateFramebuffers();
        uint32_t RefreshMultiple(uint32_t refreshMillihertz);
        void WaitForRoom();
        void Submit(const Passes& passes, const ResolvedTexture* image, uint32_t syncInterval);

        Context& context_;
        bool vsync_;
        std::unique_ptr<plume::RenderSwapChain> swapChain_;
        std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffers_;
        bool presentFailed_ = false;
        bool roomTimedOut_ = false;
        // Presents not yet matched by a wait on the frame latency object. The object starts with one
        // count, which stands for a present made before the first.
        uint32_t unmatchedPresents_ = 1;
        uint32_t refreshMillihertz_ = ~0u; // the rate last logged
        // The ramp as the present pass reads it: per entry red, green, blue (10 bits) and a pad.
        std::array<uint32_t, 4 * 256> ramp_{};
    };
}
