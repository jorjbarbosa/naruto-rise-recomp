#pragma once

// The data watch: a hardware data breakpoint on one guest word, to find the guest functions that read
// and write it. Each F8 press watches the view's rendering counter (view+0x964, step 0 finding 1) from the
// next delivery for 8 main steps, on every host thread, and writes <exe>/diagnostics/data-watch-<n>.txt:
// per host instruction that touched the word, the guest function (symbol), the thread, the reads and
// writes inside and outside the view block, and the call stack of its first hit.
//
// ROAN_DATA_WATCH=1 turns it on (the purity probe must be off: both use F8);
// ROAN_DATA_WATCH_ADDRESS=<hex> watches that guest word instead of the counter. The calls come from
// engine_hooks.cpp on the thread that runs the main step.

#include <cstdint>

namespace game::interpolation::watch
{
    // Whether the watch is on (read once).
    bool Enabled();

    // The main step starts and ends.
    void MainStepStart();
    void MainStepEnd();

    // The view block starts (view setup) and ends (after the delivery of view).
    void ViewBlockStart();
    void ViewBlockEnd(uint32_t view);
}
