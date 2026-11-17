#pragma once

// The motion record (state interpolation plan, section 10): per display of the main step, the camera
// (with its field of view), the rendered objects with dynamics, some bones of the skinned objects near
// them, the objects with modifier 5 (the look-at icons: position and scale), the view clock's times and
// what the transforms and the pose wrote, written to
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

    // The main step starts and ends. iteration is the loop iteration's index, tick whether it runs the
    // logic (without, it runs only the view block: state interpolation step 2), alpha the image time
    // past the last tick in periods.
    void MainStepStart(uint64_t iteration, bool tick, float alpha);
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
