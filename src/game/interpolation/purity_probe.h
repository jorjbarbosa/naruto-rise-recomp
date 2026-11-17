#pragma once

// The display purity probe (state interpolation plan, section 10; design in the step 0 document of
// the plan's folder), in one of two modes, each run by an F8 press:
// - the replay (ROAN_PURITY_PROBE=1): which game memory the view block writes, and which words it
//   changes again when it runs a second time in the same tick with the dt at 0, so state the display
//   advances per call. On the next 8 main steps with a view block; <exe>/diagnostics/purity-<n>.csv.
// - the carry (ROAN_CARRY_PROBE=1; step 6 document): the plan's check of invariant 2. Which words of the
//   pages the tick's view block wrote are different when the next tick's logic starts from what they
//   were when the tick ended: what the view blocks between the ticks leave to the logic, which the
//   console's single view block would not. Under the gate, on the next 8 ticks whose interval has a
//   view block, then on 8 more with the transforms suspended (the gate alone, transforms::Suspend): a
//   word changed by every interval of the first half and by none of the second is the interpolation's
//   doing, one changed by both is the view block's per frame. <exe>/diagnostics/carry-<n>.csv, the
//   same columns ("replays" counts the first half's intervals) and "off" (the second half's).
// Both repeat a noise window (other threads, as long as what they measure) and write a summary in the
// log. When both variables are set, the replay runs.
//
// Main thread only: the calls come from engine_hooks.cpp, from the main loop's first call to the exit
// of the main step.

#include <ppc_config.h>
#include <ppc_context.h>

#include <cstdint>

namespace game::interpolation::purity
{
    // Whether a probe is on (read once).
    bool Enabled();

    // A main loop iteration starts (its first call, the HUD map), and whether it runs the logic.
    void IterationStart(bool logic);

    // The main step starts.
    void MainStepStart();

    // The main step's view block starts (its view setup) and ends (after the delivery).
    void ViewBlockStart();
    void ViewBlockEnd();

    // The display renders an object (825E28D0).
    void NoteRendered(uint32_t object);

    // The display of the view block ended: the labels are built from the rendered objects here,
    // before the delivery can destroy any of them.
    void DisplayEnd();

    // The main step ended: the replay compares, replays and reports when the view block of this step
    // was taken; the carry takes the tick's state.
    void MainStepEnd(PPCContext& ctx, uint8_t* base);
}
