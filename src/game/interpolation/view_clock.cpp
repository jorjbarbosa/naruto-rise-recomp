// The view clock: the dt and the image time of the view blocks under the gate (view_clock.h).

#include "game/interpolation/view_clock.h"

#include "game/addresses.h"
#include "game/interpolation/clock.h"
#include "game/interpolation/world.h"
#include "kernel/kernel.h"
#include "memory.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <immintrin.h>

namespace game::interpolation::view_clock
{
    namespace
    {
        // The clock drops image time beyond two periods past the last tick, so no view block can owe
        // more; the clamp only bounds a view block that follows iterations without one (views off).
        constexpr double kMaxAdvance = 2.0;
        // A game time step between two ticks above this many periods is a discontinuity, not a tick.
        constexpr double kMaxTickStep = 4.0;

        struct Dt
        {
            uint64_t micro = 0;   // +56
            uint32_t scaled = 0;  // +64, float bits
            uint32_t raw = 0;     // +68, float bits

            bool operator==(const Dt&) const = default;
        };

        // The game time and the lock accumulator (+48, +96).
        struct GameTime
        {
            uint64_t time = 0, locked = 0;
        };

        Dt LoadDt(uint32_t time)
        {
            return { mem::Load64(time + jade::tim::kDt), mem::Load32(time + jade::tim::kDtScaled),
                mem::Load32(time + jade::tim::kDtRaw) };
        }

        void StoreDt(uint32_t time, const Dt& dt)
        {
            mem::Store64(time + jade::tim::kDt, dt.micro);
            mem::Store32(time + jade::tim::kDtScaled, dt.scaled);
            mem::Store32(time + jade::tim::kDtRaw, dt.raw);
        }

        GameTime LoadGameTime(uint32_t time)
        {
            return { mem::Load64(time + jade::tim::kGameTime), mem::Load64(time + jade::tim::kLocked) };
        }

        void StoreGameTime(uint32_t time, const GameTime& value)
        {
            mem::Store64(time + jade::tim::kGameTime, value.time);
            mem::Store64(time + jade::tim::kLocked, value.locked);
        }

        uint32_t Scale(uint32_t bits, double factor)
        {
            return std::bit_cast<uint32_t>(float(double(std::bit_cast<float>(bits)) * factor));
        }

        uint64_t Lerp(uint64_t from, uint64_t to, double alpha)
        {
            return from + uint64_t(std::llround(double(int64_t(to - from)) * alpha));
        }

        // Main thread state.
        Totals g_totals;
        uint32_t g_timeline = 0; // the timeline of the previous view block under the gate
        double g_image = 0;      // its image time, in periods
        uint32_t g_time = 0;     // the time structure written, 0 when nothing is written
        double g_advance = 0;    // the current view block's advance
        bool g_lastBlockGated = false; // the last view block's dt and image time were the view clock's
        Dt g_tick;               // the tick's dt, put back at the end
        Dt g_written;            // what the view block got
        // The game time at the last two ticks the view clock saw, and the last one's place.
        GameTime g_lastTick, g_previousTick;
        uint32_t g_lastTickTimeline = 0;
        uint64_t g_lastTickIndex = 0;
        GameTime g_imageTime;    // the current view block's image time
        double g_alpha = 0;      // its image position past the last tick, periods

        // The exclusion of the TimeScopes and the render thread's readers (RenderReadScope): each side
        // raises its counter, then looks at the other's (sequentially consistent, so at least one of
        // two sides starting together sees the other). A scope that sees a read backs off and waits; a
        // read waits for the scopes to end. A side waits at most this long, then goes on.
        constexpr auto kExclusionWait = std::chrono::milliseconds(2);
        std::atomic<bool> g_used = false;     // a view block was under the gate: scopes can write
        std::atomic<int> g_scopeDepth = 0;    // the main thread's TimeScopes that write
        std::atomic<int> g_renderReading = 0; // the readers running on other threads
        std::atomic<uint64_t> g_renderReads = 0, g_renderWaits = 0, g_renderOverlaps = 0, g_scopeWaits = 0;

        // Spins until value is 0 or the deadline passes; returns whether it is 0.
        bool WaitForZero(const std::atomic<int>& value, std::chrono::steady_clock::time_point deadline)
        {
            while (value.load() != 0)
            {
                if (std::chrono::steady_clock::now() >= deadline)
                    return false;
                _mm_pause();
            }
            return true;
        }

        // A TimeScope that writes enters the exclusion.
        void EnterScope()
        {
            // Nested in a scope: reads are already held off.
            if (g_scopeDepth.load() != 0)
            {
                g_scopeDepth.fetch_add(1);
                return;
            }
            // One ceiling for the whole entry, however many reads it lets go first.
            auto deadline = std::chrono::steady_clock::now() + kExclusionWait;
            bool waited = false;
            for (;;)
            {
                waited |= g_renderReading.load() != 0;
                bool clear = WaitForZero(g_renderReading, deadline);
                g_scopeDepth.fetch_add(1);
                if (!clear)
                {
                    g_renderOverlaps.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                if (g_renderReading.load() == 0)
                    break;
                // A read started meanwhile: let it go first.
                g_scopeDepth.fetch_sub(1);
                waited = true;
            }
            if (waited)
                g_scopeWaits.fetch_add(1, std::memory_order_relaxed);
        }

        // Takes the tick's game time when the logic ran since the last view block, and computes the
        // image time between the previous tick's and this one's.
        void UpdateImageTime(uint32_t time, const clock::Frame& frame)
        {
            if (frame.timeline != g_lastTickTimeline || frame.ticks != g_lastTickIndex)
            {
                GameTime current = LoadGameTime(time);
                // The previous tick's values serve when the ticks are consecutive and the game time
                // moved forward by a plausible step (a world change or a reset could move it otherwise).
                uint64_t maxStep = uint64_t(double(frame.period) * kMaxTickStep * 1e6);
                bool consecutive = frame.timeline == g_lastTickTimeline && frame.ticks == g_lastTickIndex + 1
                    && current.time >= g_lastTick.time && current.time - g_lastTick.time <= maxStep;
                // Without them (a new timeline, a tick without a view block, a jump) the previous tick
                // is this one minus its dt (+56, with the time scale), with the lock accumulator unchanged.
                g_previousTick = consecutive ? g_lastTick
                    : GameTime{ current.time - std::min(current.time, g_tick.micro), current.locked };
                g_lastTick = current;
                g_lastTickTimeline = frame.timeline;
                g_lastTickIndex = frame.ticks;
            }
            // The view clock's image (monotonic), past the last tick.
            g_alpha = std::clamp(g_image - double(frame.ticks), 0.0, 1.0);
            g_imageTime = { Lerp(g_previousTick.time, g_lastTick.time, g_alpha),
                Lerp(g_previousTick.locked, g_lastTick.locked, g_alpha) };
        }
    }

    void ViewBlockStart()
    {
        ViewBlockEnd();
        const clock::Frame& frame = clock::Current();
        uint32_t time = TimeStructure();
        g_lastBlockGated = false;
        if (!frame.gate || time == 0)
            return;

        // The first view block of a timeline has no previous image: it gets the tick's dt, as at the
        // game's own rate. Within a timeline the image never goes back: a frame the clock resyncs to a
        // slightly earlier time (presented early) keeps the previous image, with an advance of 0.
        // A gap above kMaxAdvance (ticks without a view block) gets the capped advance and drops the
        // rest: the image moves to the frame's, so the next view blocks do not pay it back.
        double image = double(frame.ticks) + double(frame.alpha);
        bool sameTimeline = frame.timeline == g_timeline;
        double gap = image - g_image;
        double advance = sameTimeline ? std::clamp(gap, 0.0, kMaxAdvance) : 1.0;
        g_timeline = frame.timeline;
        g_image = !sameTimeline || gap > kMaxAdvance ? image : g_image + advance;
        g_used.store(true);
        g_lastBlockGated = true;

        g_time = time;
        g_advance = advance;
        g_tick = LoadDt(time);
        g_written = { uint64_t(std::llround(double(g_tick.micro) * advance)), Scale(g_tick.scaled, advance),
            Scale(g_tick.raw, advance) };
        StoreDt(time, g_written);
        UpdateImageTime(time, frame);
        g_totals.viewBlocks++;
        g_totals.viewSeconds += double(std::bit_cast<float>(g_written.raw));
    }

    void ViewBlockEnd()
    {
        if (g_time == 0)
            return;
        // A field changed under the view block is someone else's write: leave it, and count it. The
        // others go back, so the next view block between ticks scales the tick's values again.
        Dt now = LoadDt(g_time);
        Dt restored = { now.micro == g_written.micro ? g_tick.micro : now.micro,
            now.scaled == g_written.scaled ? g_tick.scaled : now.scaled,
            now.raw == g_written.raw ? g_tick.raw : now.raw };
        if (!(now == g_written))
            g_totals.foreign++;
        StoreDt(g_time, restored);
        g_time = 0;
    }

    double Advance()
    {
        return g_time != 0 ? g_advance : 1.0;
    }

    TimeScope::TimeScope(Time time)
    {
        // Outside an image scope the game time is already the tick's.
        if (g_time == 0 || (time == Time::kTick && g_scopeDepth.load() == 0)
            || kernel::CurrentThread() != kernel::MainThread())
            return;
        time_ = g_time;
        GameTime saved = LoadGameTime(time_);
        gameTime_ = saved.time;
        locked_ = saved.locked;
        EnterScope();
        StoreGameTime(time_, time == Time::kImage ? g_imageTime : g_lastTick);
        (time == Time::kImage ? g_totals.imageScopes : g_totals.tickScopes)++;
    }

    TimeScope::~TimeScope()
    {
        if (time_ == 0)
            return;
        StoreGameTime(time_, { gameTime_, locked_ });
        g_scopeDepth.fetch_sub(1);
    }

    RenderReadScope::RenderReadScope()
        : excludes_(g_used.load() && kernel::CurrentThread() != kernel::MainThread())
    {
        // Before the first view block under the gate no scope can write; on the main thread a reader
        // runs between or inside its own scopes: nothing to exclude.
        if (!excludes_)
            return;
        g_renderReads.fetch_add(1, std::memory_order_relaxed);
        g_renderReading.fetch_add(1);
        if (g_scopeDepth.load() != 0)
        {
            g_renderWaits.fetch_add(1, std::memory_order_relaxed);
            if (!WaitForZero(g_scopeDepth, std::chrono::steady_clock::now() + kExclusionWait))
                g_renderOverlaps.fetch_add(1, std::memory_order_relaxed);
        }
    }

    RenderReadScope::~RenderReadScope()
    {
        if (excludes_)
            g_renderReading.fetch_sub(1);
    }

    ReplayScope::ReplayScope()
    {
        if (!g_lastBlockGated || g_time != 0)
            return;
        g_time = TimeStructure();
        g_advance = 0.0;
        active_ = g_time != 0;
    }

    ReplayScope::~ReplayScope()
    {
        if (active_)
            g_time = 0;
    }

    Times CurrentTimes()
    {
        return { g_time != 0, g_imageTime.time, g_lastTick.time, g_alpha };
    }

    Totals Totals::operator-(const Totals& start) const
    {
        return { viewSeconds - start.viewSeconds, viewBlocks - start.viewBlocks, foreign - start.foreign,
            imageScopes - start.imageScopes, tickScopes - start.tickScopes, renderReads - start.renderReads,
            renderWaits - start.renderWaits, renderOverlaps - start.renderOverlaps, scopeWaits - start.scopeWaits };
    }

    Totals Current()
    {
        Totals totals = g_totals;
        totals.renderReads = g_renderReads.load(std::memory_order_relaxed);
        totals.renderWaits = g_renderWaits.load(std::memory_order_relaxed);
        totals.renderOverlaps = g_renderOverlaps.load(std::memory_order_relaxed);
        totals.scopeWaits = g_scopeWaits.load(std::memory_order_relaxed);
        return totals;
    }
}
