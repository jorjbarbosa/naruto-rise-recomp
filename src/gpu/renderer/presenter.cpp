// The swap chain and the present of the front buffer.

#include "gpu/renderer/presenter.h"

#include "gpu/renderer/context.h"
#include "gpu/renderer/passes.h"
#include "gpu/renderer/targets.h"
#include "gpu/settings.h"
#include "log.h"

#include <plume_d3d12.h>

#include <algorithm>
#include <cmath>

namespace gpu
{
    namespace
    {
        constexpr uint32_t kSwapBuffers = 3;
        constexpr uint32_t kFrameLatency = 1;
        // Longest wait for the swap chain to accept another frame.
        constexpr DWORD kLatencyWaitMs = 100;
    }

    std::unique_ptr<Presenter> Presenter::Create(Context& context, void* window, const Settings& settings)
    {
        std::unique_ptr<Presenter> presenter(new Presenter(context));
        presenter->swapChain_ = context.Queue().createSwapChain(plume::RenderSwapChainDesc(
            static_cast<plume::RenderWindow>(window), kFormat, kSwapBuffers, false, kFrameLatency));
        if (static_cast<plume::D3D12SwapChain*>(presenter->swapChain_.get())->d3d == nullptr)
        {
            Log("gpu: error: cannot create the swap chain");
            return nullptr;
        }
        presenter->swapChain_->setVsyncEnabled(settings.vsync);
        presenter->CreateFramebuffers();
        Log("gpu: swap chain %ux%u, vsync %s", presenter->swapChain_->getWidth(), presenter->swapChain_->getHeight(),
            settings.vsync ? "on" : "off");
        return presenter;
    }

    void Presenter::CreateFramebuffers()
    {
        framebuffers_.clear();
        for (uint32_t i = 0; i < swapChain_->getTextureCount(); i++)
        {
            const plume::RenderTexture* attachment = swapChain_->getTexture(i);
            framebuffers_.push_back(context_.Device().createFramebuffer(plume::RenderFramebufferDesc(&attachment, 1)));
        }
    }

    void Presenter::Present(const Passes& passes, const ResolvedTexture* image)
    {
        if (swapChain_->needsResize())
        {
            context_.WaitIdle();
            framebuffers_.clear();
            if (swapChain_->resize())
                CreateFramebuffers();
            else if (swapChain_->getWidth() != 0 && swapChain_->getHeight() != 0)
                Log("gpu: swap chain resize to %ux%u failed", swapChain_->getWidth(), swapChain_->getHeight());
        }
        if (swapChain_->isEmpty() || framebuffers_.empty())
        {
            context_.Flush(); // minimized: the frame's work still runs
            return;
        }

        // Frame latency: wait (bounded) until the swap chain queue has room for this frame.
        auto* native = static_cast<plume::D3D12SwapChain*>(swapChain_.get());
        if (native->waitableObject != nullptr)
            WaitForSingleObject(native->waitableObject, kLatencyWaitMs);

        uint32_t index = 0;
        swapChain_->acquireTexture(nullptr, &index);
        plume::RenderTexture* backBuffer = swapChain_->getTexture(index);
        plume::RenderCommandList& list = context_.Commands();
        if (image != nullptr)
            list.barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(image->texture.get(), plume::RenderTextureLayout::SHADER_READ));
        list.barriers(plume::RenderBarrierStage::GRAPHICS,
            plume::RenderTextureBarrier(backBuffer, plume::RenderTextureLayout::COLOR_WRITE));
        list.setFramebuffer(framebuffers_[index].get());
        list.clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
        if (image != nullptr)
        {
            // The largest rectangle of the image's aspect ratio, centered.
            uint32_t width = swapChain_->getWidth();
            uint32_t height = swapChain_->getHeight();
            double scale = std::min(double(width) / image->width, double(height) / image->height);
            uint32_t fitWidth = std::clamp(uint32_t(std::lround(image->width * scale)), 1u, width);
            uint32_t fitHeight = std::clamp(uint32_t(std::lround(image->height * scale)), 1u, height);
            uint32_t x = (width - fitWidth) / 2;
            uint32_t y = (height - fitHeight) / 2;
            const plume::RenderViewport viewport{ float(x), float(y), float(fitWidth), float(fitHeight) };
            const plume::RenderRect scissor(int32_t(x), int32_t(y), int32_t(x + fitWidth), int32_t(y + fitHeight));
            list.setViewports(&viewport, 1);
            list.setScissors(&scissor, 1);
            passes.Present(list, *image->sampling);
        }
        list.barriers(plume::RenderBarrierStage::NONE,
            plume::RenderTextureBarrier(backBuffer, plume::RenderTextureLayout::PRESENT));
        context_.Flush();

        if (!swapChain_->present(index, nullptr, 0) && !presentFailed_)
        {
            presentFailed_ = true;
            Log("gpu: Present failed");
        }
    }
}
