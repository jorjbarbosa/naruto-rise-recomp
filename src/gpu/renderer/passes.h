#pragma once

#include <plume_render_interface.h>

#include <cstdint>
#include <memory>

namespace gpu
{
    // The renderer's own full-screen passes: the present and the resolves a copy cannot do. Their
    // shaders are compiled at build time (src/gpu/renderer/shaders).
    class Passes
    {
    public:
        enum class Resolve
        {
            ColorSample,      // one sample of a multisampled RGBA8 surface
            Depth,            // the depth of a single-sampled D32S8 surface, to R32_FLOAT
            DepthMultisample, // the depth of one sample of a multisampled D32S8 surface
        };

        Passes(plume::RenderDevice& device, plume::RenderFormat presentFormat);

        // A descriptor set with one texture (its view: null for the default one) for the passes.
        std::unique_ptr<plume::RenderDescriptorSet> CreateTextureSet(const plume::RenderTexture& texture,
            const plume::RenderTextureView* view) const;

        // Draws the image over the bound framebuffer's viewport.
        void Present(plume::RenderCommandList& list, plume::RenderDescriptorSet& image) const;
        // Draws over the bound framebuffer's viewport: each pixel reads the source at its position
        // plus the offset.
        void DrawResolve(plume::RenderCommandList& list, Resolve kind, plume::RenderDescriptorSet& source,
            int32_t offsetX, int32_t offsetY, uint32_t sample) const;

    private:
        plume::RenderDevice& device_;
        std::unique_ptr<plume::RenderSampler> sampler_;
        std::unique_ptr<plume::RenderPipelineLayout> presentLayout_;
        std::unique_ptr<plume::RenderPipelineLayout> resolveLayout_;
        std::unique_ptr<plume::RenderPipeline> present_;
        std::unique_ptr<plume::RenderPipeline> colorSample_;
        std::unique_ptr<plume::RenderPipeline> depth_;
        std::unique_ptr<plume::RenderPipeline> depthMultisample_;
    };
}
