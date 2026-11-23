// The present and resolve passes: pipelines over the shaders compiled at build time.

#include "gpu/renderer/passes.h"

#include "gpu/renderer/backend.h"
#include "gpu/renderer/context.h"
#include "log.h"

#include "shaders/fullscreen_vs_spirv.h"
#include "shaders/present_ps_spirv.h"
#include "shaders/resolve_color_ms_ps_spirv.h"
#include "shaders/resolve_depth_ms_ps_spirv.h"
#include "shaders/resolve_depth_ps_spirv.h"
#ifdef ROAN_D3D12
#include "shaders/fullscreen_vs.h"
#include "shaders/present_ps.h"
#include "shaders/resolve_color_ms_ps.h"
#include "shaders/resolve_depth_ms_ps.h"
#include "shaders/resolve_depth_ps.h"
#endif

#include <cstring>
#include <span>
#include <vector>

// The DXIL and SPIR-V of a pass shader by its array name (CMakeLists.txt, add_gpu_shader); the DXIL
// is empty where D3D12 is not built.
#ifdef ROAN_D3D12
#define PASS_SHADER_CODE(name) std::span<const unsigned char>(name), std::span<const unsigned char>(name##Spirv)
#else
#define PASS_SHADER_CODE(name) std::span<const unsigned char>(), std::span<const unsigned char>(name##Spirv)
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

        const plume::RenderDescriptorRange kTextureRange(plume::RenderDescriptorRangeType::TEXTURE, 0, 1);
        const plume::RenderDescriptorSetDesc kTextureSet(&kTextureRange, 1);

        // A pass shader in the device's format, from the DXIL and SPIR-V headers of the build
        // (PASS_SHADER_CODE).
        std::unique_ptr<plume::RenderShader> Shader(plume::RenderDevice& device, std::span<const unsigned char> dxil,
            std::span<const unsigned char> spirv)
        {
            if (ShaderFormat(device) == plume::RenderShaderFormat::DXIL)
            {
                if (dxil.empty())
                    Fatal("gpu: this build has no DXIL pass shaders");
                return device.createShader(dxil.data(), dxil.size(), "main", plume::RenderShaderFormat::DXIL);
            }
            // The header's bytes have no alignment, and Vulkan reads SPIR-V as 32-bit words.
            std::vector<uint32_t> words((spirv.size() + 3) / 4);
            memcpy(words.data(), spirv.data(), spirv.size());
            return device.createShader(words.data(), spirv.size(), "main", plume::RenderShaderFormat::SPIRV);
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

        auto fullscreen = Shader(device, PASS_SHADER_CODE(kFullscreenVs));
        present_ = Pipeline(device, *presentLayout_, *fullscreen, *Shader(device, PASS_SHADER_CODE(kPresentPs)), presentFormat,
            "present");
        colorSample_ = Pipeline(device, *resolveLayout_, *fullscreen, *Shader(device, PASS_SHADER_CODE(kResolveColorMsPs)),
            plume::RenderFormat::R8G8B8A8_UNORM, "color sample resolve");
        depth_ = Pipeline(device, *resolveLayout_, *fullscreen, *Shader(device, PASS_SHADER_CODE(kResolveDepthPs)),
            plume::RenderFormat::R32_FLOAT, "depth resolve");
        depthMultisample_ = Pipeline(device, *resolveLayout_, *fullscreen,
            *Shader(device, PASS_SHADER_CODE(kResolveDepthMsPs)), plume::RenderFormat::R32_FLOAT,
            "multisampled depth resolve");
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
}
