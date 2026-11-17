#pragma once

// The data watch: hardware data breakpoints on up to four guest words, to find the guest functions that
// read and write them. Each F8 press watches the view's rendering counter (view+0x964, step 0 finding 1)
// from the next delivery for 8 main steps, on every host thread, and writes
// <exe>/diagnostics/data-watch-<n>.txt: per word and host instruction that touched it, the guest
// function (symbol), the thread, the reads and writes inside and outside the view block, and the call
// stack of its first hit.
//
// ROAN_DATA_WATCH=1 turns it on (the purity probe must be off: both use F8);
// ROAN_DATA_WATCH_ADDRESS=<hex>[,<hex>...] watches up to four guest words instead of the counter;
// ROAN_DATA_WATCH_OBJECT=<hex offset>[,...] (with the addresses, four words in all) the words at those
// offsets of the first object with dynamics the display rendered in the view block before the watch.
// The calls come from engine_hooks.cpp on the thread that runs the main step.

#include <cstdint>

namespace game::interpolation::watch
{
    // Whether the watch is on (read once).
    bool Enabled();

    // The main step starts and ends.
    void MainStepStart();
    void MainStepEnd();

    // The view block starts (view setup) and ends (after the delivery of view, which ran the sector switch when
    // sectorSwitch).
    void ViewBlockStart();
    void ViewBlockEnd(uint32_t view, bool sectorSwitch);

    // The display renders an object (825E28D0, r6), inside the main step's display.
    void NoteRendered(uint32_t object);
}
