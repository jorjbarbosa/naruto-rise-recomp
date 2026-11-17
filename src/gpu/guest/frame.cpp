// The frame: D3DDevice_Swap runs as the original and reaches the host through VdSwap, which presents
// the front buffer (the texture the title resolved its frame into) and paces the game, by the
// display's refreshes or by a QPC limiter. The frame batch closes in the KickOff at the end of the
// Swap, and the Swap waits for the previous frame's fence, so at most one frame is in flight.

#include "gpu/gpu.h"

#include "gpu/guest/addresses.h"
#include "gpu/guest/buffers.h"
#include "gpu/guest/device.h"
#include "gpu/guest/draw.h"
#include "gpu/guest/resources.h"
#include "gpu/guest/textures.h"
#include "gpu/guest/watch.h"
#include "gpu/renderer/renderer.h"
#include "gpu/settings.h"
#include "host/window.h"
#include "kernel/kernel.h"

#include <windows.h>

namespace gpu
{
    namespace
    {
        // D3DPRESENT_INTERVAL_IMMEDIATE: present without waiting.
        constexpr uint32_t kIntervalImmediate = 0x80000000;
        constexpr uint32_t kMaxInterval = 4;

        std::unique_ptr<Renderer> g_renderer;

        // The title's presentation interval in 60 Hz vblanks; 0 for D3DPRESENT_INTERVAL_IMMEDIATE.
        uint32_t VblankInterval(uint32_t interval)
        {
            static uint32_t last = ~0u;
            if (interval != last)
            {
                Log("gpu: presentation interval %u", interval);
                last = interval;
            }
            if ((interval & kIntervalImmediate) != 0)
                return 0;
            // D3DPRESENT_INTERVAL_DEFAULT (0) waits for one vblank, like D3DPRESENT_INTERVAL_ONE.
            if (interval == 0)
                return 1;
            if (interval > kMaxInterval)
            {
                LOG_ONCE("gpu: presentation interval %u clamped to %u", interval, kMaxInterval);
                return kMaxInterval;
            }
            return interval;
        }

        // Paces the frames with the QPC clock when the display cannot: one frame per given number of
        // 60 Hz vblanks, none for 0. A frame that arrives early waits for its deadline; a late one
        // starts a new phase instead of catching up. Render thread only.
        class FrameLimiter
        {
        public:
            void Pace(uint32_t vblanks)
            {
                if (vblanks != vblanks_)
                {
                    vblanks_ = vblanks;
                    next_ = 0;
                }
                if (vblanks == 0)
                    return;
                if (frequency_ == 0)
                {
                    LARGE_INTEGER frequency;
                    QueryPerformanceFrequency(&frequency);
                    frequency_ = frequency.QuadPart;
                    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
                }

                int64_t period = int64_t(vblanks) * frequency_ / kVblankRate;
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
            uint32_t vblanks_ = ~0u;
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
        Settings settings = Settings::FromEnvironment();
        wchar_t executable[MAX_PATH];
        DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
        if (length != 0 && length < MAX_PATH) // MAX_PATH: the path was cut short
            settings.pipelineList = std::filesystem::path(executable).parent_path() / "cache" / "pipelines.bin";
        g_renderer = Renderer::Create(window, settings,
            [](uint32_t fence) { guest::GetDevice().Publish(fence); }, [] { guest::FlushInlineDraw(); });
        if (!g_renderer)
            Log("gpu: error: running without a renderer");
    }

    void FreePhysicalMemory(uint32_t address, uint32_t size)
    {
        guest::ForgetMemory(address, size);
        guest::ForgetBuffers(guest::GpuAddress(address), guest::GpuAddress(address) + size);
    }

    void Swap(uint32_t frontBufferFetch)
    {
        // The front buffer is a texture the title resolved its frame into.
        guest::TextureFetch front = guest::ReadTextureFetch(frontBufferFetch);
        LOG_ONCE("gpu: front buffer at %08X, %ux%u, format %u", front.address, front.width, front.height, front.format);

        // A ramp the title set since the last Swap (D3DDevice_SetGammaRamp, through the Options
        // brightness) applies from this frame on.
        uint32_t device = guest::GetDevice().Address();
        if (g_renderer && device != 0 &&
            (mem::Load32(device + guest::dev::kSwapPending) & guest::dev::kSwapPendingGamma) != 0)
        {
            GammaRamp ramp;
            for (uint32_t i = 0; i < ramp.size(); i++)
                ramp[i] = mem::Load16(device + guest::dev::kGammaRamp + 2 * i);
            g_renderer->SetGammaRamp(ramp);
        }

        // One clock: the display when its refresh rate is a multiple of 60 Hz (each frame then stays
        // the interval's vblanks on screen, and the next frame starts once the previous one is
        // shown), the limiter otherwise. The wait is only for a renderer whose display paced the
        // last frame; otherwise the frame's tail of owner work overlaps the next frame as before.
        uint32_t vblanks = VblankInterval(device != 0 ? mem::Load32(device + guest::dev::kPresentInterval) : 1);
        uint64_t present = g_renderer ? g_renderer->Present(front.address, vblanks, host::DisplayRefreshMillihertz()) : 0;
        guest::EndTextureFrame();
        guest::EndBufferFrame();
        guest::EndWatchFrame();
        bool paced = g_renderer && g_renderer->DisplayPaces() && g_renderer->WaitPresented(present);
        g_limiter.Pace(paced ? 0 : vblanks);
    }
}
