#pragma once

// The motion record (state interpolation plan, section 10): per display of the main step, the camera,
// the rendered objects with dynamics and some bones of the skinned objects near them, written to
// <exe>/diagnostics/motion.csv; every 120 main steps the rates go to the log. tools/motion_stats.py
// of the plan's folder measures from it how evenly the image moves.
//
// ROAN_MOTION_RECORD=1 turns it on. Main thread only: the calls come from engine_hooks.cpp, between
// the entry and the exit of the main step.

#include <cstdint>

namespace game::interpolation::motion
{
    // Whether the record is on (read once).
    bool Enabled();

    // The main step starts (iteration: its index) and ends.
    void MainStepStart(uint64_t iteration);
    void MainStepEnd();

    // The display of the main step's view block starts and ends.
    void DisplayStart(uint32_t view);
    void DisplayEnd();

    // The display renders an object (825E28D0). The actors (objects with dynamics) are taken from
    // these, at their matrix as the display draws it.
    void NoteRendered(uint32_t object);

    // The display copies a bone matrix of the object being rendered into the frame's packet.
    void NoteBone(uint32_t bone, uint32_t matrix);
}
