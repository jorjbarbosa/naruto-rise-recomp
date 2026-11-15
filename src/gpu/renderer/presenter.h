#pragma once

#include "gpu/renderer/renderer.h"

#include <plume_render_interface.h>

#include <array>
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
        void Present(const Passes& passes, const ResolvedTexture* image);

    private:
        explicit Presenter(Context& context);
        void CreateFramebuffers();

        Context& context_;
        std::unique_ptr<plume::RenderSwapChain> swapChain_;
        std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffers_;
        bool presentFailed_ = false;
        // The ramp as the present pass reads it: per entry red, green, blue (10 bits) and a pad.
        std::array<uint32_t, 4 * 256> ramp_{};
    };
}
