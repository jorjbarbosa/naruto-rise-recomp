#pragma once

#include <plume_render_interface.h>

#include <cstdint>
#include <memory>

namespace gpu
{
    class Context;

    // The renderer's own full-screen passes: the present and the resolves a copy cannot do. Their
    // shaders are compiled at build time (src/gpu/renderer/shaders). Each pass claims the command
    // list's graphics state (Context::ClaimGraphicsState). Owner thread only.
    class Passes
    {
    public:
        enum class Resolve
        {
            ColorSample,      // one sample of a multisampled RGBA8 surface
            Depth,            // the depth of a single-sampled D32S8 surface, to R32_FLOAT
            DepthMultisample, // the depth of one sample of a multisampled D32S8 surface
        };

        Passes(Context& context, plume::RenderFormat presentFormat);

        // A descriptor set with one texture (its view: null for the default one) for the passes.
        std::unique_ptr<plume::RenderDescriptorSet> CreateTextureSet(const plume::RenderTexture& texture,
            const plume::RenderTextureView* view) const;

        // The present's gamma ramp: 256 entries of (red, green, blue, unused) 10-bit values, a
        // constant buffer of this many bytes.
        static constexpr uint32_t kRampBytes = 256 * 16;

        // Draws the image through the ramp (kRampBytes in a buffer the list reads, readable by
        // reference: backend.h, ConstantReferences) over the bound framebuffer's viewport.
        void Present(plume::RenderCommandList& list, plume::RenderDescriptorSet& image,
            plume::RenderBufferReference ramp) const;
        // Draws over the bound framebuffer's viewport: each pixel reads the source at its position
        // plus the offset.
        void DrawResolve(plume::RenderCommandList& list, Resolve kind, plume::RenderDescriptorSet& source,
            int32_t offsetX, int32_t offsetY, uint32_t sample) const;

    private:
        Context& context_;
        plume::RenderDevice& device_;
        std::unique_ptr<plume::RenderSampler> sampler_;
        // The present's sampler set, which holds the immutable sampler (BindImmutableSamplers).
        std::unique_ptr<plume::RenderDescriptorSet> samplerSet_;
        std::unique_ptr<plume::RenderPipelineLayout> presentLayout_;
        std::unique_ptr<plume::RenderPipelineLayout> resolveLayout_;
        std::unique_ptr<plume::RenderPipeline> present_;
        std::unique_ptr<plume::RenderPipeline> colorSample_;
        std::unique_ptr<plume::RenderPipeline> depth_;
        std::unique_ptr<plume::RenderPipeline> depthMultisample_;
    };
}
