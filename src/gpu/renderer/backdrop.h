#pragma once

#include <plume_render_interface.h>

#include <cstdint>
#include <memory>

namespace gpu
{
    class Context;
    class Passes;
    struct ResolvedTexture;

    // The blurred game image behind the host UI: the image through the gamma ramp at half its size,
    // halved again, then blurred in two directions. Owner thread only.
    class Backdrop
    {
    public:
        explicit Backdrop(Context& context) : context_(context) {}

        // Makes the blurred image of this image (ramp: Passes::kRampBytes the list reads), which
        // Compose draws until the next Prepare. Changes the bound framebuffer, viewport and scissor.
        void Prepare(plume::RenderCommandList& list, const Passes& passes, const ResolvedTexture& image,
            plume::RenderBufferReference ramp);
        // Draws the image Prepare made over the bound framebuffer's viewport, with the alpha.
        void Compose(plume::RenderCommandList& list, const Passes& passes, float alpha);

    private:
        struct Target
        {
            uint32_t width = 0;
            uint32_t height = 0;
            std::unique_ptr<plume::RenderTexture> texture;
            std::unique_ptr<plume::RenderFramebuffer> framebuffer;
            std::unique_ptr<plume::RenderDescriptorSet> sampling;
        };

        // Makes the targets for an image of this size.
        void Create(const Passes& passes, uint32_t width, uint32_t height);
        void CreateTarget(const Passes& passes, Target& target, uint32_t width, uint32_t height);
        // Binds the target to draw into over its whole area.
        void Begin(plume::RenderCommandList& list, Target& target);
        void End(plume::RenderCommandList& list, Target& target);

        Context& context_;
        uint32_t width_ = 0; // the image's size the targets were made for
        uint32_t height_ = 0;
        Target half_;
        Target quarter_;    // the half halved, then the blurred image (the vertical pass)
        Target horizontal_; // the horizontal pass
    };
}
