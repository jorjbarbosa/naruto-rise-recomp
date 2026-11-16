// The swap chain, the present of the front buffer and the display clock that paces it.

#include "gpu/renderer/presenter.h"

#include "gpu/renderer/context.h"
#include "gpu/renderer/passes.h"
#include "gpu/renderer/targets.h"
#include "gpu/settings.h"
#include "log.h"

#include <plume_d3d12.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <windows.h>

namespace gpu
{
    namespace
    {
        constexpr uint32_t kSwapBuffers = 3;
        constexpr uint32_t kFrameLatency = 1;
        // Longest wait for the swap chain to accept another frame.
        constexpr DWORD kLatencyWaitMs = 100;
        // DXGI's largest sync interval.
        constexpr uint32_t kMaxSyncInterval = 4;
        // A refresh rate within 1 Hz of a multiple of 60 Hz counts as that multiple (59.94 Hz, 239.76 Hz).
        constexpr uint32_t kRateToleranceMillihertz = 1000;
    }

    Presenter::Presenter(Context& context, bool vsync) : context_(context), vsync_(vsync)
    {
        // The identity ramp: 8-bit index i to 10-bit i * 1023 / 255, which the 8-bit back buffer
        // rounds back to i.
        for (uint32_t i = 0; i < 256; i++)
            for (uint32_t channel = 0; channel < 3; channel++)
                ramp_[4 * i + channel] = (i * 1023 + 127) / 255;
    }

    void Presenter::SetGammaRamp(const GammaRamp& ramp)
    {
        for (uint32_t channel = 0; channel < 3; channel++)
            for (uint32_t i = 0; i < 256; i++)
                ramp_[4 * i + channel] = ramp[256 * channel + i] >> kGammaRampShift;
    }

    std::unique_ptr<Presenter> Presenter::Create(Context& context, void* window, const Settings& settings)
    {
        std::unique_ptr<Presenter> presenter(new Presenter(context, settings.vsync));
        presenter->swapChain_ = context.Queue().createSwapChain(plume::RenderSwapChainDesc(
            static_cast<plume::RenderWindow>(window), kFormat, kSwapBuffers, false, kFrameLatency));
        if (static_cast<plume::D3D12SwapChain*>(presenter->swapChain_.get())->d3d == nullptr)
        {
            Log("gpu: error: cannot create the swap chain");
            return nullptr;
        }
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

    uint32_t Presenter::RefreshMultiple(uint32_t refreshMillihertz)
    {
        constexpr uint32_t kGuestMillihertz = kVblankRate * 1000;
        uint32_t multiple = (refreshMillihertz + kGuestMillihertz / 2) / kGuestMillihertz;
        if (multiple == 0 ||
            uint32_t(std::abs(int64_t(refreshMillihertz) - int64_t(multiple) * kGuestMillihertz)) > kRateToleranceMillihertz)
            multiple = 0;
        if (refreshMillihertz != refreshMillihertz_)
        {
            refreshMillihertz_ = refreshMillihertz;
            double hertz = refreshMillihertz / 1000.0;
            if (!vsync_)
                Log("gpu: display at %.3f Hz, frames paced by the limiter (vsync off)", hertz);
            else if (multiple != 0)
                Log("gpu: display at %.3f Hz, frames paced by the display (%u refreshes per 60 Hz vblank)", hertz, multiple);
            else
                Log("gpu: display at %.3f Hz, not a multiple of 60 Hz: frames paced by the limiter", hertz);
        }
        return vsync_ ? multiple : 0;
    }

    void Presenter::WaitForRoom()
    {
        // The frame latency object counts the presents that left the queue (shown or replaced). One
        // wait per present keeps the count matched, so with a latency of one this returns once the
        // last present is on screen. A wait that times out stays owed, so the late signal of its
        // present is taken by the next wait instead of releasing a later one early.
        auto* native = static_cast<plume::D3D12SwapChain*>(swapChain_.get());
        if (native->waitableObject == nullptr)
            return;
        while (unmatchedPresents_ != 0)
        {
            if (WaitForSingleObject(native->waitableObject, kLatencyWaitMs) == WAIT_TIMEOUT)
            {
                if (!roomTimedOut_)
                {
                    roomTimedOut_ = true;
                    Log("gpu: the swap chain took longer than %u ms to accept a frame", unsigned(kLatencyWaitMs));
                }
                return;
            }
            unmatchedPresents_--;
        }
    }

    void Presenter::Submit(const Passes& passes, const ResolvedTexture* image, uint32_t syncInterval)
    {
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
            static_assert(sizeof(ramp_) == Passes::kRampBytes);
            Context::Upload ramp = context_.Allocate(Passes::kRampBytes, Context::kConstantAlignment);
            memcpy(ramp.data, ramp_.data(), sizeof(ramp_));
            passes.Present(list, *image->sampling, ramp.Reference());
        }
        list.barriers(plume::RenderBarrierStage::NONE,
            plume::RenderTextureBarrier(backBuffer, plume::RenderTextureLayout::PRESENT));
        context_.Flush();

        // Tearing needs the swap chain flag and is only asked for with vsync off.
        auto* native = static_cast<plume::D3D12SwapChain*>(swapChain_.get());
        UINT flags = syncInterval == 0 && !vsync_ && (native->swapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0
            ? DXGI_PRESENT_ALLOW_TEARING : 0;
        unmatchedPresents_++;
        if (FAILED(native->d3d->Present(syncInterval, flags)) && !presentFailed_)
        {
            presentFailed_ = true;
            Log("gpu: Present failed");
        }
    }

    void Presenter::Present(const Passes& passes, const ResolvedTexture* image, uint32_t vblanks,
        uint32_t refreshMillihertz, const std::function<void(bool paced)>& shown)
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
            // Minimized: the frame's work still runs, and the caller paces itself.
            context_.Flush();
            shown(false);
            return;
        }

        uint32_t refreshes = vblanks * RefreshMultiple(refreshMillihertz);
        if (refreshes == 0)
        {
            WaitForRoom();
            Submit(passes, image, vsync_ ? 1 : 0);
            shown(false);
            return;
        }
        // The first present goes once the previous image is on screen, and the caller is released
        // then: it makes its next frame while this one is held and drawn by the GPU, one frame ahead
        // of the display. The other presents of a long hold follow as the queue frees, keeping the
        // owner here until all but the last are on screen.
        uint32_t presents = (refreshes + kMaxSyncInterval - 1) / kMaxSyncInterval;
        for (uint32_t i = 0; i < presents; i++)
        {
            WaitForRoom();
            Submit(passes, image, refreshes / presents + (i < refreshes % presents ? 1 : 0));
            if (i == 0)
                shown(true);
        }
    }
}
