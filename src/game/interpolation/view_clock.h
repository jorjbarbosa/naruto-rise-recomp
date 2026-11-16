#pragma once

// The view clock (state interpolation plan, section 8.5; step 3 document of the plan's folder): the
// time the view block sees while the gate shows the game above its own rate.
//
// The dt. The logic sets the time structure's dt (+56 µs, +64 scaled seconds, +68 raw seconds) once
// per tick. The view block runs once per frame, and what the display integrates by the dt (the
// material animators, the fades, the modifiers that accumulate +64) would advance a whole tick per
// frame. So from the view setup to the end of the delivery, every view block of the main step (the
// tick's and those between ticks) sees the tick's dt times the image's advance since the previous view
// block, in periods (clock::Frame::image): over a tick the view blocks add up to the tick's dt, as the
// engine's own double rendering divides its dt across its displays. The tick's values are put back
// after the delivery where the stored values are still there.
//
// The image time. The display's phase consumers (modifier Applies, the grass) compute from the game
// time (+48, 64-bit µs) minus the lock accumulator (+96, 64-bit µs). The image time is the pair the
// frame shows: the previous tick's values plus alpha times their advance to the current tick's. It is
// not written for the whole view block: the render thread reads the time structure live (screen
// effects), so the image time is written only around each consumer's call (TimeScope), by the hooks of
// time_consumers.cpp, and the tick's values are put back when it returns. The render thread's readers
// and the scopes exclude each other (RenderReadScope), so those readers keep the tick's time.
//
// Off (the clock off, or the gate not deciding) nothing is written. Main thread only, unless a function
// says otherwise: the calls come from engine_hooks.cpp and time_consumers.cpp, inside the main step.

#include <cstdint>

namespace game::interpolation::view_clock
{
    // The view setup of a main step's view block: writes the dt for the image's advance and computes
    // the image time.
    void ViewBlockStart();

    // The end of the delivery, and the end of the main step (in case a view block did not reach its
    // delivery): puts the tick's dt back. Nothing to do when ViewBlockStart did not write.
    void ViewBlockEnd();

    // The image's advance the current view block was given, in periods (0 in a ReplayScope).
    double Advance();

    // The purity probe's replay of the view block (after the main step, with the originals and the dt
    // at 0): for its duration the TimeScopes write the image time of the view block it replays, with
    // an advance of 0, so the replay computes the phases the view block computed. Does nothing when
    // that view block was not the view clock's.
    class ReplayScope
    {
    public:
        ReplayScope();
        ~ReplayScope();
        ReplayScope(const ReplayScope&) = delete;
        ReplayScope& operator=(const ReplayScope&) = delete;

    private:
        bool active_ = false;
    };

    // Which time a TimeScope gives the code it brackets.
    enum class Time
    {
        kImage, // the image time: phases the frame shows
        kTick,  // the tick's: consumers that must keep it inside an image scope (the wind field)
    };

    // Writes the chosen game time and lock accumulator (+48, +96) for its lifetime, and puts back what
    // was there when it ends. Scopes nest. Does nothing outside an active view block or off the main
    // thread (the engine also displays on the loading screen's thread), nor for the tick's time outside
    // an image scope (it is already there).
    class TimeScope
    {
    public:
        explicit TimeScope(Time time);
        ~TimeScope();
        TimeScope(const TimeScope&) = delete;
        TimeScope& operator=(const TimeScope&) = delete;

        // Whether the scope wrote (an active view block, on the main thread).
        bool Writes() const { return time_ != 0; }

    private:
        uint32_t time_ = 0; // the time structure written, 0 when the scope does nothing
        uint64_t gameTime_ = 0, locked_ = 0;
    };

    // Brackets a render-thread reader of the game time (the screen effects read it live while the main
    // thread displays): it waits until no TimeScope is writing, and no TimeScope starts until it ends,
    // so the reader sees the tick's game time as before. (The dt it would see is the view block's, as
    // in step 2, where it was 0 between ticks; none of the readers found reads it.) Each side waits at
    // most kExclusionWait (view_clock.cpp) in all, then goes on and the overlap is counted: neither can
    // block the other for good, whatever locks the guest holds. Nothing to do on the main thread, or
    // before the first view block under the gate. Any thread.
    class RenderReadScope
    {
    public:
        RenderReadScope();
        ~RenderReadScope();
        RenderReadScope(const RenderReadScope&) = delete;
        RenderReadScope& operator=(const RenderReadScope&) = delete;

    private:
        bool excludes_; // off the main thread
    };

    // The current view block's image and tick game times (+48, µs), for the motion record.
    struct Times
    {
        bool active = false;
        uint64_t image = 0, tick = 0;
    };
    Times CurrentTimes();

    // Totals since the start, for the clock's report.
    struct Totals
    {
        double viewSeconds = 0;  // the raw dt (+68) the view blocks saw while the gate decided
        uint64_t viewBlocks = 0; // view blocks whose dt was written
        uint64_t foreign = 0;    // restores that found another value in the dt (a writer to name)
        uint64_t imageScopes = 0, tickScopes = 0; // TimeScopes that wrote
        // RenderReadScope: reads; reads that waited for a TimeScope; reads that ran during one (the wait's
        // ceiling passed on either side; must stay at 0); TimeScopes that waited for a read.
        uint64_t renderReads = 0, renderWaits = 0, renderOverlaps = 0, scopeWaits = 0;

        // The counts since start.
        Totals operator-(const Totals& start) const;
    };
    Totals Current();
}
