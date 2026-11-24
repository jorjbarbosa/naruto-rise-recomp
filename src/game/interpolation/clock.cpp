// The tick clock and the gate (clock.h).

#include "game/interpolation/clock.h"

#include "game/addresses.h"
#include "game/destruction_order.h"
#include "game/interpolation/display_writes.h"
#include "game/interpolation/lifetime.h"
#include "game/interpolation/pose.h"
#include "game/interpolation/transforms.h"
#include "game/interpolation/view_block.h"
#include "game/interpolation/view_clock.h"
#include "game/interpolation/world.h"
#include "gpu/gpu.h"
#include "host/timing.h"
#include "log.h"
#include "memory.h"
#include "settings/settings.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <string>

namespace game::interpolation::clock
{
    namespace
    {
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
            transforms::Totals transforms; // transforms::Current() at the start
            pose::Totals pose;             // pose::Current() at the start
            display_writes::Totals display; // display_writes::Current() at the start
            lifetime::Totals lifetime;     // lifetime::Current() at the start
        };
        Window g_window;

        int64_t g_rate = -1;     // video.frame_rate, read once, then RequestRate's; 0: off
        constexpr int64_t kNoRequest = -1;
        std::atomic<int64_t> g_requestedRate{ kNoRequest };

        double Seconds()
        {
            int64_t now = host::Counter();
            if (g_frequency == 0)
            {
                g_frequency = host::CounterFrequency();
                g_origin = now;
            }
            return double(now - g_origin) / double(g_frequency);
        }

        // Waits until Seconds() reaches target.
        void SleepUntil(double target)
        {
            Seconds(); // sets the origin
            host::SleepUntil(g_origin + int64_t(std::ceil(target * double(g_frequency))));
        }

        void StartWindow(double now)
        {
            uint32_t time = TimeStructure();
            g_window = {};
            g_window.start = now;
            g_window.gameTime = time != 0 ? mem::Load64(time + jade::tim::kGameTime) : 0;
            g_window.sectorSwitches = destruction::SectorSwitches();
            g_window.view = view_clock::Current();
            g_window.transforms = transforms::Current();
            g_window.pose = pose::Current();
            g_window.display = display_writes::Current();
            g_window.lifetime = lifetime::Current();
        }

        void ReportTransforms(double seconds)
        {
            const transforms::Totals t = transforms::Current() - g_window.transforms;
            lifetime::Totals now = lifetime::Current();
            auto count = [](uint64_t value) { return (unsigned long long)value; };

            if (display_writes::Enabled())
            {
                const display_writes::Totals d = display_writes::Current() - g_window.display;
                Log("display writes: %.1f s: %llu view blocks settled, hierarchy globals %llu (%llu changed), locals "
                    "%llu (%llu changed), skeleton objects %llu (%llu changed), %llu floats changed, max shift since "
                    "the start %.4f, %llu left freed, %llu unclassified (callers %08X %08X %08X %08X)",
                    seconds, count(d.blocks), count(d.globals), count(d.globalsChanged), count(d.locals),
                    count(d.localsChanged), count(d.children), count(d.childrenChanged), count(d.floatsChanged),
                    d.maxShift, count(d.freed), count(d.unknown), d.unknownCallers[0], d.unknownCallers[1],
                    d.unknownCallers[2], d.unknownCallers[3]);
            }
            Log("transforms: %.1f s: %llu view blocks, objects %.1f written per block (%llu held at a cut, %llu with "
                "display floats left, %llu changed by the display, %llu in translation), %llu tracks started, %llu "
                "captures, cuts %llu jump, %llu turn, %llu matrix, %llu invalid, %llu scale bit changes blended, %llu fast steps kept (max "
                "step since the start %.2f), %llu dropped (freed), %llu floats not restored (freed); camera %llu written (%llu held, "
                "%llu changed), cuts %llu move, %llu father, %llu viewport, %llu invalid, %llu fast steps kept; heap "
                "headers %llu bad, reports lost %llu",
                seconds, count(t.blocks), t.blocks != 0 ? double(t.written) / double(t.blocks) : 0.0, count(t.held),
                count(t.partial), count(t.changed), count(t.changedTranslation), count(t.starts), count(t.captures),
                count(t.cutJump), count(t.cutTurn), count(t.cutMatrix), count(t.cutInvalid), count(t.scaleChanges),
                count(t.fastKept), t.maxStep, count(t.dropped), count(t.unrestored), count(t.cameraWritten),
                count(t.cameraHeld), count(t.cameraChanged), count(t.cameraCutMove), count(t.cameraCutFather),
                count(t.cameraCutViewport), count(t.cameraCutInvalid), count(t.cameraFastKept),
                count(now.badHeaders - g_window.lifetime.badHeaders),
                count(now.overflows - g_window.lifetime.overflows));

            if (!pose::Enabled())
                return;
            const pose::Totals p = pose::Current() - g_window.pose;
            Log("pose: %.1f s: %llu copies blended, %llu held, %.1f bones per copy, %llu with another object's bones, "
                "%llu untracked; %llu "
                "skeletons started, %llu captures, cuts %llu jump, %llu turn (%llu holding only the pose), %llu "
                "bones from the display's walk, at the tick %llu layout, %llu "
                "invalid; bones left %llu display, %llu moved, %llu shared, %llu floats not restored; largest step "
                "per skeleton and tick (<0.05/0.1/0.25/0.5/1/2/more) %llu/%llu/%llu/%llu/%llu/%llu/%llu, turn "
                "(<10/20/30/45/60/90/more degrees) %llu/%llu/%llu/%llu/%llu/%llu/%llu, max since the start %.2f, %.0f "
                "degrees",
                seconds, count(p.blended), count(p.held),
                p.blended + p.held != 0 ? double(p.bones) / double(p.blended + p.held) : 0.0, count(p.borrowed),
                count(p.untracked),
                count(p.starts), count(p.captures), count(p.cutJump), count(p.cutTurn), count(p.poseHeld),
                count(p.walked), count(p.tickLayout),
                count(p.tickInvalid), count(p.display), count(p.moved), count(p.shared), count(p.unrestored),
                count(p.steps[0]), count(p.steps[1]), count(p.steps[2]), count(p.steps[3]), count(p.steps[4]),
                count(p.steps[5]), count(p.steps[6]), count(p.turns[0]), count(p.turns[1]), count(p.turns[2]),
                count(p.turns[3]), count(p.turns[4]), count(p.turns[5]), count(p.turns[6]), p.maxStep, p.maxTurn);
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
            if (view.viewBlocks != 0)
                ReportTransforms(seconds);
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
            g_rate = int64_t(::settings::FrameRate());
        }
        return g_rate != 0;
    }

    void RequestRate(uint32_t rate)
    {
        g_requestedRate.store(int64_t(rate), std::memory_order_release);
    }

    void IterationStart()
    {
        int64_t rate = g_requestedRate.exchange(kNoRequest, std::memory_order_acquire);
        if (rate == kNoRequest)
            return;
        Enabled(); // the setting's rate, if this is the first iteration
        if (rate == g_rate)
            return;
        bool was = g_rate != 0;
        g_rate = rate;
        g_decided = false;
        g_started = false; // the timeline starts again with a tick
        if (rate == 0)
        {
            // Off: every iteration a tick, paced at the title's interval again.
            if (was)
            {
                double now = Seconds();
                if (g_window.iterations != 0)
                    Report(now);
                gpu::SetPaceRate(0);
            }
            uint32_t timeline = g_frame.timeline; // a later timeline is still a new one
            g_frame = Frame{};
            g_frame.timeline = timeline;
            g_pace = ~0u;
        }
        Log("clock: frame rate %s", rate == 0 ? "off (the game's own)" : std::to_string(rate).c_str());
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
