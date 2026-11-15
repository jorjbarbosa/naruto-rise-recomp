// The present and resolve passes: pipelines over the shaders compiled at build time.

#include "gpu/renderer/passes.h"

#include "log.h"

#include <plume_d3d12.h>

#include "shaders/fullscreen_vs.h"
#include "shaders/present_ps.h"
#include "shaders/resolve_color_ms_ps.h"
#include "shaders/resolve_depth_ms_ps.h"
#include "shaders/resolve_depth_ps.h"

namespace gpu
{
    namespace
    {
        // The gamma ramp of the present (present_ps.hlsl): a root constant buffer, b0 in space 2.
        constexpr uint32_t kRampRoot = 0;
        constexpr uint32_t kRampSpace = 2;

        // Root constants of the resolve pass (resolve_ps.hlsl).
        struct ResolveConstants
        {
            int32_t offsetX;
            int32_t offsetY;
            uint32_t sample;
        };

        const plume::RenderDescriptorRange kTextureRange(plume::RenderDescriptorRangeType::TEXTURE, 0, 1);
        const plume::RenderDescriptorSetDesc kTextureSet(&kTextureRange, 1);

        template <size_t N>
        std::unique_ptr<plume::RenderShader> Shader(plume::RenderDevice& device, const unsigned char (&blob)[N])
        {
            return device.createShader(blob, N, "main", plume::RenderShaderFormat::DXIL);
        }

        std::unique_ptr<plume::RenderPipeline> Pipeline(plume::RenderDevice& device, const plume::RenderPipelineLayout& layout,
            const plume::RenderShader& vertex, const plume::RenderShader& pixel, plume::RenderFormat format, const char* name)
        {
            plume::RenderGraphicsPipelineDesc desc;
            desc.pipelineLayout = &layout;
            desc.vertexShader = &vertex;
            desc.pixelShader = &pixel;
            desc.renderTargetFormat[0] = format;
            desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
            desc.renderTargetCount = 1;
            std::unique_ptr<plume::RenderPipeline> pipeline = device.createGraphicsPipeline(desc);
            if (static_cast<plume::D3D12GraphicsPipeline*>(pipeline.get())->d3d == nullptr)
                Fatal("gpu: cannot create the %s pipeline", name);
            return pipeline;
        }
    }

    Passes::Passes(plume::RenderDevice& device, plume::RenderFormat presentFormat) : device_(device)
    {
        plume::RenderSamplerDesc sampler;
        sampler.minFilter = plume::RenderFilter::LINEAR;
        sampler.magFilter = plume::RenderFilter::LINEAR;
        sampler.mipmapMode = plume::RenderMipmapMode::NEAREST;
        sampler.addressU = plume::RenderTextureAddressMode::CLAMP;
        sampler.addressV = plume::RenderTextureAddressMode::CLAMP;
        sampler.addressW = plume::RenderTextureAddressMode::CLAMP;
        sampler.shaderVisibility = plume::RenderShaderVisibility::PIXEL;
        sampler_ = device.createSampler(sampler);

        // Present: the image in set 0, the sampler (immutable; the pass gathers with its clamp) in
        // set 1, the gamma ramp as a root constant buffer.
        const plume::RenderSampler* samplers[] = { sampler_.get() };
        const plume::RenderDescriptorRange samplerRange(plume::RenderDescriptorRangeType::SAMPLER, 0, 1, samplers);
        const plume::RenderDescriptorSetDesc presentSets[] = { kTextureSet, plume::RenderDescriptorSetDesc(&samplerRange, 1) };
        const plume::RenderRootDescriptorDesc ramp(kRampRoot, kRampSpace, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
        plume::RenderPipelineLayoutDesc presentDesc(nullptr, 0, presentSets, 2);
        presentDesc.rootDescriptorDescs = &ramp;
        presentDesc.rootDescriptorDescsCount = 1;
        presentLayout_ = device.createPipelineLayout(presentDesc);

        // Resolve: the constants, then the source in set 0.
        const plume::RenderPushConstantRange constants(0, 0, 0, sizeof(ResolveConstants), plume::RenderShaderStageFlag::PIXEL);
        resolveLayout_ = device.createPipelineLayout(plume::RenderPipelineLayoutDesc(&constants, 1, &kTextureSet, 1));

        auto fullscreen = Shader(device, kFullscreenVs);
        present_ = Pipeline(device, *presentLayout_, *fullscreen, *Shader(device, kPresentPs), presentFormat, "present");
        colorSample_ = Pipeline(device, *resolveLayout_, *fullscreen, *Shader(device, kResolveColorMsPs),
            plume::RenderFormat::R8G8B8A8_UNORM, "color sample resolve");
        depth_ = Pipeline(device, *resolveLayout_, *fullscreen, *Shader(device, kResolveDepthPs),
            plume::RenderFormat::R32_FLOAT, "depth resolve");
        depthMultisample_ = Pipeline(device, *resolveLayout_, *fullscreen, *Shader(device, kResolveDepthMsPs),
            plume::RenderFormat::R32_FLOAT, "multisampled depth resolve");
    }

    std::unique_ptr<plume::RenderDescriptorSet> Passes::CreateTextureSet(const plume::RenderTexture& texture,
        const plume::RenderTextureView* view) const
    {
        std::unique_ptr<plume::RenderDescriptorSet> set = device_.createDescriptorSet(kTextureSet);
        set->setTexture(0, &texture, plume::RenderTextureLayout::SHADER_READ, view);
        return set;
    }

    void Passes::Present(plume::RenderCommandList& list, plume::RenderDescriptorSet& image,
        plume::RenderBufferReference ramp) const
    {
        list.setGraphicsPipelineLayout(presentLayout_.get());
        list.setPipeline(present_.get());
        list.setGraphicsDescriptorSet(&image, 0);
        list.setGraphicsRootDescriptor(ramp, kRampRoot);
        list.drawInstanced(3, 1, 0, 0);
    }

    void Passes::DrawResolve(plume::RenderCommandList& list, Resolve kind, plume::RenderDescriptorSet& source,
        int32_t offsetX, int32_t offsetY, uint32_t sample) const
    {
        const plume::RenderPipeline* pipeline = kind == Resolve::ColorSample ? colorSample_.get()
            : kind == Resolve::Depth                                         ? depth_.get()
                                                                             : depthMultisample_.get();
        const ResolveConstants constants{ offsetX, offsetY, sample };
        list.setGraphicsPipelineLayout(resolveLayout_.get());
        list.setPipeline(pipeline);
        list.setGraphicsPushConstants(0, &constants, 0, sizeof(constants));
        list.setGraphicsDescriptorSet(&source, 0);
        list.drawInstanced(3, 1, 0, 0);
    }
}
