// The tick clock and the gate (clock.h).

#include "game/interpolation/clock.h"

#include "game/addresses.h"
#include "game/destruction_order.h"
#include "game/interpolation/view_block.h"
#include "game/interpolation/view_clock.h"
#include "game/interpolation/world.h"
#include "gpu/gpu.h"
#include "log.h"
#include "memory.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <windows.h>

namespace game::interpolation::clock
{
    namespace
    {
        constexpr uint32_t kMinRate = 31;
        constexpr uint32_t kMaxRate = 240;
        // A valid fixed period, seconds.
        constexpr float kMinPeriod = 1e-4f;
        constexpr float kMaxPeriod = 0.25f;
        // The gate needs a period of at least this many frames.
        constexpr double kGateFrames = 1.25;
        // Tolerance of the time comparisons, seconds: the period is a float (2/60 is 0.033333335).
        constexpr double kEpsilon = 1e-6;
        // The image time may run this many periods ahead of the last tick before the excess is dropped.
        constexpr double kMaxBehind = 2.0;
        constexpr double kReportSeconds = 10.0;

        // Main thread state.
        Frame g_frame;
        bool g_decided = false;  // the current iteration was decided
        bool g_started = false;  // the gate's timeline runs
        double g_frameTime = 0;  // image time of this iteration's frame, seconds since g_origin
        double g_tickTime = 0;   // image time of the last tick
        uint32_t g_pace = ~0u;   // the rate given to the limiter (0: the title's interval)
        int64_t g_frequency = 0, g_origin = 0;

        // Counters since the last report.
        struct Window
        {
            double start = 0;
            uint64_t iterations = 0, ticks = 0;
            uint64_t resyncs = 0;       // frames more than half a frame off their slot
            uint64_t drops = 0;         // iterations whose image time ran more than kMaxBehind periods ahead
            double dropped = 0;         // the image time dropped, seconds
            uint64_t noViewBlock = 0;   // iterations without a tick or a view block
            double simulated = 0;       // the ticks' raw dt (+68), seconds
            uint64_t gameTime = 0;      // +48 at the start, µs
            uint64_t sectorSwitches = 0; // destruction::SectorSwitches() at the start
            view_clock::Totals view;     // view_clock::Current() at the start
        };
        Window g_window;

        int64_t g_rate = -1;     // ROAN_FRAME_RATE, read once; 0: off
        HANDLE g_timer = nullptr; // high-resolution waitable timer for SleepUntil

        double Seconds()
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            if (g_frequency == 0)
            {
                LARGE_INTEGER frequency;
                QueryPerformanceFrequency(&frequency);
                g_frequency = frequency.QuadPart;
                g_origin = now.QuadPart;
            }
            return double(now.QuadPart - g_origin) / double(g_frequency);
        }

        // Waits until Seconds() reaches target: a high-resolution timer to about 1 ms before it, then
        // a spin (a plain sleep has the system's 15.6 ms granularity).
        void SleepUntil(double target)
        {
            constexpr double kSpin = 1e-3;
            if (g_timer == nullptr)
                g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            double wait = target - Seconds() - kSpin;
            if (g_timer != nullptr && wait > 0)
            {
                LARGE_INTEGER due;
                due.QuadPart = -int64_t(wait * 1e7);
                if (SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE))
                    WaitForSingleObject(g_timer, INFINITE);
            }
            while (Seconds() < target)
                YieldProcessor();
        }

        void StartWindow(double now)
        {
            uint32_t time = TimeStructure();
            g_window = {};
            g_window.start = now;
            g_window.gameTime = time != 0 ? mem::Load64(time + jade::tim::kGameTime) : 0;
            g_window.sectorSwitches = destruction::SectorSwitches();
            g_window.view = view_clock::Current();
        }

        void Report(double now)
        {
            const Window& w = g_window;
            double seconds = now - w.start;
            if (seconds <= 0)
                return;
            uint32_t time = TimeStructure();
            uint64_t gameTime = time != 0 ? mem::Load64(time + jade::tim::kGameTime) : 0;
            uint64_t switches = destruction::SectorSwitches() - w.sectorSwitches;
            const view_clock::Totals view = view_clock::Current() - w.view;
            Log("clock: %.1f s: %.2f iterations/s, %.2f ticks/s, simulated %.4f s/s, view %.4f s/s in %llu view blocks "
                "(%llu foreign dt), image time scopes %llu (tick %llu, waited %llu), render thread reads %llu "
                "(waited %llu, overlaps %llu), "
                "game time %.4f s/s, sector switches %.3f per tick, resyncs %llu, drops %llu (%.1f ms), without a "
                "view block %llu; gate %s, period %.6f s, pace %u",
                seconds, double(w.iterations) / seconds, double(w.ticks) / seconds, w.simulated / seconds,
                view.viewSeconds / seconds, (unsigned long long)view.viewBlocks, (unsigned long long)view.foreign,
                (unsigned long long)view.imageScopes, (unsigned long long)view.tickScopes,
                (unsigned long long)view.scopeWaits, (unsigned long long)view.renderReads,
                (unsigned long long)view.renderWaits, (unsigned long long)view.renderOverlaps,
                double(int64_t(gameTime - w.gameTime)) * 1e-6 / seconds,
                w.ticks != 0 ? double(switches) / double(w.ticks) : 0.0, (unsigned long long)w.resyncs,
                (unsigned long long)w.drops, w.dropped * 1e3, (unsigned long long)w.noViewBlock,
                g_frame.gate ? "on" : "off", g_frame.period, g_pace);
            StartWindow(now);
        }

        void Decide()
        {
            double now = Seconds();
            uint32_t time = TimeStructure();
            bool fixed = time != 0 && mem::Load32(time + jade::tim::kFixedStep) != 0;
            float period = time != 0 ? LoadFloat(time + jade::tim::kPeriod) : 0.0f;
            double step = 1.0 / double(g_rate);
            bool valid = fixed && std::isfinite(period) && period > kMinPeriod && period <= kMaxPeriod;
            bool gate = valid && double(period) >= kGateFrames * step - kEpsilon;

            // Without the gate under the fixed step every iteration is a tick at the title's step: pace
            // at the tick rate, or a rate other than the tick's would change the game's speed.
            uint32_t pace = gate ? g_rate : valid ? uint32_t(std::lround(1.0 / double(period))) : 0;
            bool changed = gate != g_frame.gate || pace != g_pace || (gate && period != g_frame.period);
            if (changed)
            {
                if (g_window.iterations != 0)
                    Report(now);
                else
                    StartWindow(now);
                Log("clock: gate %s, period %.6f s, fixed step %u, frame rate %u, pace %u", gate ? "on" : "off",
                    period, fixed ? 1u : 0u, uint32_t(g_rate), pace);
                gpu::SetPaceRate(pace);
                g_pace = pace;
                // A change of the gate, the period or the pace restarts the timeline with a tick.
                g_started = false;
            }
            g_frame.gate = gate;
            g_frame.period = gate ? period : 0.0f;
            if (!gate)
            {
                g_frame.tick = true;
                g_frame.alpha = 0.0f;
            }
            else
            {
                if (!g_started)
                {
                    g_started = true;
                    g_frameTime = now;
                    g_tickTime = now - double(period);
                    g_frame.ticks = 0;
                    g_frame.timeline++;
                }
                else
                {
                    // On schedule the frames are 1 / rate apart; a frame more than half a frame off its
                    // slot (a slow frame, a load) takes the real time.
                    g_frameTime += step;
                    if (std::fabs(now - g_frameTime) > 0.5 * step)
                    {
                        g_frameTime = now;
                        g_window.resyncs++;
                    }
                }
                double behind = g_frameTime - g_tickTime;
                double limit = kMaxBehind * double(period);
                if (behind > limit)
                {
                    g_window.drops++;
                    g_window.dropped += behind - limit;
                    g_tickTime = g_frameTime - limit;
                }
                g_frame.tick = g_frameTime - g_tickTime >= double(period) - kEpsilon;
                // A tick taken within kEpsilon before its time realigns the tick time to the frame:
                // the float period is not a whole number of frames (2/60 s is 1.7 ns more than two
                // frames of 1/60 s), and without the realignment the ticks would fall behind the
                // frames by that much each until one slipped a frame (after about 600 ticks).
                if (g_frame.tick)
                {
                    g_tickTime = std::min(g_tickTime + double(period), g_frameTime);
                    g_frame.ticks++;
                }
                g_frame.alpha = float(std::clamp((g_frameTime - g_tickTime) / double(period), 0.0, 1.0));
            }
        }
    }

    bool Enabled()
    {
        if (g_rate < 0)
        {
            const char* value = std::getenv("ROAN_FRAME_RATE");
            int rate = value != nullptr ? std::atoi(value) : 0;
            if (rate > int(kMaxRate))
            {
                Log("clock: ROAN_FRAME_RATE %d above %u, using %u", rate, kMaxRate, kMaxRate);
                rate = int(kMaxRate);
            }
            g_rate = rate >= int(kMinRate) ? rate : 0;
        }
        return g_rate != 0;
    }

    const Frame& Current()
    {
        return g_frame;
    }

    bool LogicRuns()
    {
        if (!g_decided)
        {
            g_decided = true;
            Decide();
        }
        return g_frame.tick;
    }

    void RunBetweenTicks(PPCContext& ctx, uint8_t* base)
    {
        bool ran;
        {
            ExtraViewBlockState extraState(ExtraDt::kViewClock);
            ran = RunViewBlock(ctx, base, false, ViewBlockCalls::kHooked);
        }
        if (!ran)
        {
            g_window.noViewBlock++;
            SleepUntil(g_frameTime + 1.0 / double(g_rate));
        }
    }

    void IterationEnd()
    {
        g_decided = false;
        g_window.iterations++;
        if (g_frame.tick)
        {
            g_window.ticks++;
            uint32_t time = TimeStructure();
            if (time != 0)
                g_window.simulated += LoadFloat(time + jade::tim::kDtRaw);
        }
        double now = Seconds();
        if (now - g_window.start >= kReportSeconds)
            Report(now);
    }
}
