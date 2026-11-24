// The present, resolve, copy and blur passes: pipelines over the shaders compiled at build time.

#include "gpu/renderer/passes.h"

#include "gpu/renderer/backend.h"
#include "gpu/renderer/context.h"
#include "log.h"

#include "shaders/blur_ps_spirv.h"
#include "shaders/copy_ps_spirv.h"
#include "shaders/fullscreen_vs_spirv.h"
#include "shaders/present_ps_spirv.h"
#include "shaders/resolve_color_ms_ps_spirv.h"
#include "shaders/resolve_depth_ms_ps_spirv.h"
#include "shaders/resolve_depth_ps_spirv.h"
#ifdef ROAN_D3D12
#include "shaders/blur_ps.h"
#include "shaders/copy_ps.h"
#include "shaders/fullscreen_vs.h"
#include "shaders/present_ps.h"
#include "shaders/resolve_color_ms_ps.h"
#include "shaders/resolve_depth_ms_ps.h"
#include "shaders/resolve_depth_ps.h"
#endif

namespace gpu
{
    namespace
    {
        // The gamma ramp of the present (present_ps.hlsl), read by reference: b0 in space 1.
        const ConstantReferences kRamp{ 1, 1, plume::RenderShaderStageFlag::PIXEL };

        // Root constants of the resolve pass (resolve_ps.hlsl).
        struct ResolveConstants
        {
            int32_t offsetX;
            int32_t offsetY;
            uint32_t sample;
        };

        // Root constants of the copy and blur passes (copy_ps.hlsl, blur_ps.hlsl): the copy reads
        // the alpha, the blur the direction.
        struct FilterConstants
        {
            float alpha;
            int32_t directionX;
            int32_t directionY;
        };

        const plume::RenderDescriptorRange kTextureRange(plume::RenderDescriptorRangeType::TEXTURE, 0, 1);
        const plume::RenderDescriptorSetDesc kTextureSet(&kTextureRange, 1);
        const plume::RenderDescriptorRange kSamplerRange(plume::RenderDescriptorRangeType::SAMPLER, 0, 1);
        const plume::RenderDescriptorSetDesc kSamplerSet(&kSamplerRange, 1);

        std::unique_ptr<plume::RenderPipeline> Pipeline(plume::RenderDevice& device, const plume::RenderPipelineLayout& layout,
            const plume::RenderShader& vertex, const plume::RenderShader& pixel, plume::RenderFormat format, const char* name,
            const plume::RenderBlendDesc& blend = plume::RenderBlendDesc::Copy())
        {
            plume::RenderGraphicsPipelineDesc desc;
            desc.pipelineLayout = &layout;
            desc.vertexShader = &vertex;
            desc.pixelShader = &pixel;
            desc.renderTargetFormat[0] = format;
            desc.renderTargetBlend[0] = blend;
            desc.renderTargetCount = 1;
            std::unique_ptr<plume::RenderPipeline> pipeline = device.createGraphicsPipeline(desc);
            if (!IsCreated(pipeline.get()))
                Fatal("gpu: cannot create the %s pipeline", name);
            return pipeline;
        }
    }

    Passes::Passes(Context& context, plume::RenderFormat presentFormat)
        : context_(context), device_(context.Device())
    {
        plume::RenderDevice& device = device_;

        // Present: the image in set 0 (loaded, not sampled), the gamma ramp by reference.
        presentLayout_ = CreateLayout(device, plume::RenderPipelineLayoutDesc(nullptr, 0, &kTextureSet, 1), kRamp);

        // Resolve: the constants, then the source in set 0.
        const plume::RenderPushConstantRange constants(0, 0, 0, sizeof(ResolveConstants), plume::RenderShaderStageFlag::PIXEL);
        resolveLayout_ = device.createPipelineLayout(plume::RenderPipelineLayoutDesc(&constants, 1, &kTextureSet, 1));

        // Copy and blur: the constants, the source in set 0, the copy's sampler in set 1.
        plume::RenderSamplerDesc sampler;
        sampler.minFilter = plume::RenderFilter::LINEAR;
        sampler.magFilter = plume::RenderFilter::LINEAR;
        sampler.mipmapMode = plume::RenderMipmapMode::NEAREST;
        sampler.addressU = plume::RenderTextureAddressMode::CLAMP;
        sampler.addressV = plume::RenderTextureAddressMode::CLAMP;
        sampler.addressW = plume::RenderTextureAddressMode::CLAMP;
        sampler.shaderVisibility = plume::RenderShaderVisibility::PIXEL;
        sampler_ = device.createSampler(sampler);
        samplerSet_ = device.createDescriptorSet(kSamplerSet);
        samplerSet_->setSampler(0, sampler_.get());
        const plume::RenderPushConstantRange filterConstants(0, 0, 0, sizeof(FilterConstants),
            plume::RenderShaderStageFlag::PIXEL);
        const plume::RenderDescriptorSetDesc filterSets[] = { kTextureSet, kSamplerSet };
        filterLayout_ = device.createPipelineLayout(plume::RenderPipelineLayoutDesc(&filterConstants, 1, filterSets, 2));

        auto fullscreen = CreatePassShader(device, PASS_SHADER_CODE(kFullscreenVs));
        present_ = Pipeline(device, *presentLayout_, *fullscreen, *CreatePassShader(device, PASS_SHADER_CODE(kPresentPs)),
            presentFormat, "present");
        colorSample_ = Pipeline(device, *resolveLayout_, *fullscreen,
            *CreatePassShader(device, PASS_SHADER_CODE(kResolveColorMsPs)), plume::RenderFormat::R8G8B8A8_UNORM,
            "color sample resolve");
        depth_ = Pipeline(device, *resolveLayout_, *fullscreen, *CreatePassShader(device, PASS_SHADER_CODE(kResolveDepthPs)),
            plume::RenderFormat::R32_FLOAT, "depth resolve");
        depthMultisample_ = Pipeline(device, *resolveLayout_, *fullscreen,
            *CreatePassShader(device, PASS_SHADER_CODE(kResolveDepthMsPs)), plume::RenderFormat::R32_FLOAT,
            "multisampled depth resolve");
        copy_ = Pipeline(device, *filterLayout_, *fullscreen, *CreatePassShader(device, PASS_SHADER_CODE(kCopyPs)),
            presentFormat, "copy", plume::RenderBlendDesc::AlphaBlend());
        blur_ = Pipeline(device, *filterLayout_, *fullscreen, *CreatePassShader(device, PASS_SHADER_CODE(kBlurPs)),
            presentFormat, "blur");
    }

    const plume::RenderDescriptorSetDesc& Passes::TextureSetDesc()
    {
        return kTextureSet;
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
        context_.ClaimGraphicsState(this);
        list.setGraphicsPipelineLayout(presentLayout_.get());
        list.setPipeline(present_.get());
        list.setGraphicsDescriptorSet(&image, 0);
        BindConstantReferences(list, &ramp, kRamp.count, nullptr);
        list.drawInstanced(3, 1, 0, 0);
    }

    void Passes::DrawResolve(plume::RenderCommandList& list, Resolve kind, plume::RenderDescriptorSet& source,
        int32_t offsetX, int32_t offsetY, uint32_t sample) const
    {
        const plume::RenderPipeline* pipeline = kind == Resolve::ColorSample ? colorSample_.get()
            : kind == Resolve::Depth                                         ? depth_.get()
                                                                             : depthMultisample_.get();
        const ResolveConstants constants{ offsetX, offsetY, sample };
        context_.ClaimGraphicsState(this);
        list.setGraphicsPipelineLayout(resolveLayout_.get());
        list.setPipeline(pipeline);
        list.setGraphicsPushConstants(0, &constants, 0, sizeof(constants));
        list.setGraphicsDescriptorSet(&source, 0);
        list.drawInstanced(3, 1, 0, 0);
    }

    void Passes::DrawCopy(plume::RenderCommandList& list, plume::RenderDescriptorSet& source, float alpha) const
    {
        const FilterConstants constants{ alpha, 0, 0 };
        DrawFilter(list, *copy_, source, &constants, sizeof(constants));
    }

    void Passes::DrawBlur(plume::RenderCommandList& list, plume::RenderDescriptorSet& source, bool vertical) const
    {
        const FilterConstants constants{ 1.0f, vertical ? 0 : 1, vertical ? 1 : 0 };
        DrawFilter(list, *blur_, source, &constants, sizeof(constants));
    }

    void Passes::DrawFilter(plume::RenderCommandList& list, const plume::RenderPipeline& pipeline,
        plume::RenderDescriptorSet& source, const void* constants, uint32_t size) const
    {
        context_.ClaimGraphicsState(this);
        list.setGraphicsPipelineLayout(filterLayout_.get());
        list.setPipeline(&pipeline);
        list.setGraphicsPushConstants(0, constants, 0, size);
        list.setGraphicsDescriptorSet(&source, 0);
        list.setGraphicsDescriptorSet(samplerSet_.get(), 1);
        list.drawInstanced(3, 1, 0, 0);
    }
}
