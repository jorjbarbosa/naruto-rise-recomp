// The blurred game image the host UI asks for behind it (gpu::ui::Frame::blur).

#include "gpu/renderer/backdrop.h"

#include "gpu/renderer/context.h"
#include "gpu/renderer/passes.h"
#include "gpu/renderer/presenter.h"
#include "gpu/renderer/targets.h"
#include "log.h"

#include <plume_d3d12.h>

#include <algorithm>

namespace gpu
{
    void Backdrop::Create(const Passes& passes, uint32_t width, uint32_t height)
    {
        width_ = width;
        height_ = height;
        CreateTarget(passes, half_, std::max(width / 2, 1u), std::max(height / 2, 1u));
        CreateTarget(passes, quarter_, std::max(width / 4, 1u), std::max(height / 4, 1u));
        CreateTarget(passes, horizontal_, quarter_.width, quarter_.height);
    }

    void Backdrop::CreateTarget(const Passes& passes, Target& target, uint32_t width, uint32_t height)
    {
        if (target.texture)
            context_.Retire(std::make_shared<Target>(std::move(target)));
        target.width = width;
        target.height = height;
        target.texture =
            context_.Device().createTexture(plume::RenderTextureDesc::ColorTarget(width, height, Presenter::kFormat));
        if (static_cast<plume::D3D12Texture*>(target.texture.get())->d3d == nullptr)
            Fatal("gpu: cannot create a %ux%u backdrop target", width, height);
        const plume::RenderTexture* attachment = target.texture.get();
        target.framebuffer = context_.Device().createFramebuffer(plume::RenderFramebufferDesc(&attachment, 1));
        target.sampling = passes.CreateTextureSet(*target.texture, nullptr);
    }

    void Backdrop::Begin(plume::RenderCommandList& list, Target& target)
    {
        list.barriers(plume::RenderBarrierStage::GRAPHICS,
            plume::RenderTextureBarrier(target.texture.get(), plume::RenderTextureLayout::COLOR_WRITE));
        list.setFramebuffer(target.framebuffer.get());
        list.setViewports(plume::RenderViewport(0.0f, 0.0f, float(target.width), float(target.height)));
        list.setScissors(plume::RenderRect(0, 0, int32_t(target.width), int32_t(target.height)));
    }

    void Backdrop::End(plume::RenderCommandList& list, Target& target)
    {
        list.barriers(plume::RenderBarrierStage::GRAPHICS,
            plume::RenderTextureBarrier(target.texture.get(), plume::RenderTextureLayout::SHADER_READ));
    }

    void Backdrop::Prepare(plume::RenderCommandList& list, const Passes& passes, const ResolvedTexture& image,
        plume::RenderBufferReference ramp)
    {
        if (image.width != width_ || image.height != height_)
            Create(passes, image.width, image.height);
        Begin(list, half_);
        passes.Present(list, *image.sampling, ramp);
        End(list, half_);
        Begin(list, quarter_);
        passes.DrawCopy(list, *half_.sampling, 1.0f);
        End(list, quarter_);
        Begin(list, horizontal_);
        passes.DrawBlur(list, *quarter_.sampling, false);
        End(list, horizontal_);
        Begin(list, quarter_);
        passes.DrawBlur(list, *horizontal_.sampling, true);
        End(list, quarter_);
    }

    void Backdrop::Compose(plume::RenderCommandList& list, const Passes& passes, float alpha)
    {
        passes.DrawCopy(list, *quarter_.sampling, alpha);
    }
}
