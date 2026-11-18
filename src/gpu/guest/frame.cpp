// The frame: D3DDevice_Swap runs as the original and reaches the host through VdSwap, which presents
// the front buffer (the texture the title resolved its frame into) and paces the game, by the
// display's refreshes or by a QPC limiter. The frame batch closes in the KickOff at the end of the
// Swap, and the Swap waits for the previous frame's fence, so at most one frame is in flight.

#include "gpu/gpu.h"

#include "gpu/guest/addresses.h"
#include "gpu/guest/buffers.h"
#include "gpu/guest/compare.h"
#include "gpu/guest/device.h"
#include "gpu/guest/draw.h"
#include "gpu/guest/resources.h"
#include "gpu/guest/textures.h"
#include "gpu/guest/watch.h"
#include "gpu/renderer/renderer.h"
#include "gpu/settings.h"
#include "host/paths.h"
#include "host/timing.h"
#include "host/window.h"
#include "kernel/kernel.h"

#include <atomic>

namespace gpu
{
    namespace
    {
        // D3DPRESENT_INTERVAL_IMMEDIATE: present without waiting.
        constexpr uint32_t kIntervalImmediate = 0x80000000;
        constexpr uint32_t kMaxInterval = 4;

        std::unique_ptr<Renderer> g_renderer;
        std::atomic<uint32_t> g_paceRate{ 0 }; // SetPaceRate

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
        // 60 Hz vblanks, or at a rate (1 / rate s) when one is given; none for neither. A frame that
        // arrives early waits for its deadline; a late one starts a new phase instead of catching up.
        // The deadlines of a phase are computed from its start, so a period that is not a whole number
        // of clock ticks does not drift. Render thread only.
        class FrameLimiter
        {
        public:
            // Under a rate every Swap is paced at it, whatever interval the title asked for (immediate
            // included), and a change of that interval does not start a new phase.
            void Pace(uint32_t vblanks, uint32_t rate)
            {
                if (rate != rate_ || (rate == 0 && vblanks != vblanks_))
                {
                    if (rate != rate_ && rate != 0)
                        Log("gpu: pacing at %u frames/s", rate);
                    vblanks_ = vblanks;
                    rate_ = rate;
                    start_ = 0;
                }
                if (rate == 0 && vblanks == 0)
                    return;
                // The period is ticks / frames clock ticks.
                int64_t frequency = host::CounterFrequency();
                int64_t ticks = rate != 0 ? frequency : int64_t(vblanks) * frequency;
                int64_t frames = rate != 0 ? int64_t(rate) : int64_t(kVblankRate);
                int64_t now = host::Counter();
                int64_t next = start_ + (frame_ + 1) * ticks / frames;
                if (start_ != 0 && now < next)
                {
                    host::SleepUntil(next);
                    frame_++;
                }
                else
                {
                    start_ = now;
                    frame_ = 0;
                }
            }

        private:
            int64_t start_ = 0; // the start of the phase, 0 before the first frame
            int64_t frame_ = 0; // frames paced since the start of the phase
            uint32_t vblanks_ = ~0u;
            uint32_t rate_ = 0;
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
        Settings settings = Settings::FromEnvironment();
        if (!settings.renderer)
        {
            Log("gpu: no renderer (ROAN_GPU_API=none)");
            return;
        }
        void* window = host::MainWindowHandle();
        if (window == nullptr)
        {
            Log("gpu: error: no game window, running without a renderer");
            return;
        }
        settings.recordFrames = guest::CompareRecordsFrames();
        std::filesystem::path executable = host::ExecutableDirectory();
        if (!executable.empty())
            settings.SetCacheDirectory(executable / "cache");
        // A quad of BeginVertices still waiting for its vertices goes before anything else posted.
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
        // Under a pace rate (SetPaceRate) the limiter paces at it and the frame is presented at once:
        // the display holds a frame only for whole 60 Hz vblanks.
        uint32_t rate = g_paceRate.load(std::memory_order_relaxed);
        uint32_t vblanks = VblankInterval(device != 0 ? mem::Load32(device + guest::dev::kPresentInterval) : 1);
        uint64_t present = g_renderer
            ? g_renderer->Present(front.address, rate != 0 ? 0 : vblanks, host::DisplayRefreshMillihertz())
            : 0;
        guest::EndTextureFrame();
        guest::EndBufferFrame();
        guest::EndWatchFrame();
        guest::EndCompareFrame(g_renderer.get());
        bool paced = rate == 0 && g_renderer && g_renderer->DisplayPaces() && g_renderer->WaitPresented(present);
        g_limiter.Pace(paced ? 0 : vblanks, rate);
    }

    void SetPaceRate(uint32_t rate)
    {
        g_paceRate.store(rate, std::memory_order_relaxed);
    }
}
