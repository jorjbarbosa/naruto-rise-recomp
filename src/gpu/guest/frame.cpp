// The frame: D3DDevice_Swap runs as the original and reaches the host through VdSwap, which presents
// the front buffer (the texture the title resolved its frame into) and paces the game. The frame
// batch closes in the KickOff at the end of the Swap, and the Swap waits for the previous frame's
// fence, so at most one frame is in flight.

#include "gpu/gpu.h"

#include "gpu/guest/addresses.h"
#include "gpu/guest/device.h"
#include "gpu/guest/draw.h"
#include "gpu/guest/resources.h"
#include "gpu/guest/textures.h"
#include "gpu/renderer/renderer.h"
#include "gpu/settings.h"
#include "host/window.h"
#include "kernel/kernel.h"

#include <windows.h>

namespace gpu
{
    namespace
    {
        constexpr uint32_t kVblankRate = 60;
        // D3DPRESENT_INTERVAL_IMMEDIATE: present without waiting.
        constexpr uint32_t kIntervalImmediate = 0x80000000;
        constexpr uint32_t kMaxInterval = 4;

        std::unique_ptr<Renderer> g_renderer;

        // Paces the frames at the presentation interval. A frame that arrives early waits for its
        // deadline; a late one starts a new phase instead of catching up. Render thread only.
        class FrameLimiter
        {
        public:
            void Pace(uint32_t interval)
            {
                if (interval != interval_)
                {
                    Log("gpu: presentation interval %u", interval);
                    interval_ = interval;
                    next_ = 0;
                }
                if ((interval & kIntervalImmediate) != 0)
                    return;
                // D3DPRESENT_INTERVAL_DEFAULT (0) waits for one vblank, like D3DPRESENT_INTERVAL_ONE.
                if (interval == 0)
                    interval = 1;
                if (interval > kMaxInterval)
                {
                    LOG_ONCE("gpu: presentation interval %u clamped to %u", interval, kMaxInterval);
                    interval = kMaxInterval;
                }
                if (frequency_ == 0)
                {
                    LARGE_INTEGER frequency;
                    QueryPerformanceFrequency(&frequency);
                    frequency_ = frequency.QuadPart;
                    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
                }

                int64_t period = int64_t(interval) * frequency_ / kVblankRate;
                int64_t now = Now();
                if (next_ != 0 && now < next_)
                {
                    // Sleep to about 1 ms before the deadline, then spin.
                    int64_t margin = frequency_ / 1000;
                    if (timer_ != nullptr && next_ - now > margin)
                    {
                        LARGE_INTEGER due;
                        due.QuadPart = -((next_ - now - margin) * 10'000'000 / frequency_);
                        if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE))
                            WaitForSingleObject(timer_, INFINITE);
                    }
                    while (Now() < next_)
                        YieldProcessor();
                    next_ += period;
                }
                else
                {
                    next_ = now + period;
                }
            }

        private:
            static int64_t Now()
            {
                LARGE_INTEGER counter;
                QueryPerformanceCounter(&counter);
                return counter.QuadPart;
            }

            int64_t frequency_ = 0;
            int64_t next_ = 0;
            uint32_t interval_ = ~0u;
            HANDLE timer_ = nullptr;
        };

        FrameLimiter g_limiter;
    }

    Renderer* ActiveRenderer()
    {
        return g_renderer.get();
    }

    void InitializeEngines()
    {
        if (g_renderer)
            return;
        void* window = host::MainWindowHandle();
        if (window == nullptr)
        {
            Log("gpu: error: no game window, running without a renderer");
            return;
        }
        // A quad of BeginVertices still waiting for its vertices goes before anything else posted.
        g_renderer = Renderer::Create(window, Settings::FromEnvironment(),
            [](uint32_t fence) { guest::GetDevice().Publish(fence); }, [] { guest::FlushInlineDraw(); });
        if (!g_renderer)
            Log("gpu: error: running without a renderer");
    }

    void FreePhysicalMemory(uint32_t address, uint32_t size)
    {
        guest::ForgetMemory(address, size);
    }

    void Swap(uint32_t frontBufferFetch)
    {
        // The front buffer is a texture the title resolved its frame into.
        guest::TextureFetch front = guest::ReadTextureFetch(frontBufferFetch);
        LOG_ONCE("gpu: front buffer at %08X, %ux%u, format %u", front.address, front.width, front.height, front.format);

        uint32_t device = guest::GetDevice().Address();
        if (device != 0 && (mem::Load32(device + guest::dev::kSwapPending) & guest::dev::kSwapPendingGamma) != 0)
            LOG_ONCE("gpu: the title set a gamma ramp; it is not applied yet");

        if (g_renderer)
            g_renderer->Present(front.address);
        guest::EndTextureFrame();
        g_limiter.Pace(device != 0 ? mem::Load32(device + guest::dev::kPresentInterval) : 1);
    }
}
