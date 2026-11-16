#pragma once

// The display purity probe (state interpolation plan, section 10; design in the step 0 document of
// the plan's folder): which game memory the view block writes, and which words it changes again when
// it runs a second time in the same tick with the dt at 0, so state the display advances per call.
// Each F8 press runs it on the next 8 main steps with a view block and writes
// <exe>/diagnostics/purity-<n>.csv and a summary in the log.
//
// ROAN_PURITY_PROBE=1 turns it on. Main thread only: the calls come from engine_hooks.cpp, between
// the entry and the exit of the main step.

#include <ppc_config.h>
#include <ppc_context.h>

#include <cstdint>

namespace game::interpolation::purity
{
    // Whether the probe is on (read once).
    bool Enabled();

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

    // The main step ended: compares, replays and reports when the view block of this step was taken.
    void MainStepEnd(PPCContext& ctx, uint8_t* base);
}
