#pragma once

// The order between the title's destructions and the Jade render thread (state interpolation plan,
// F1 and section 8.4; design and measurements in the step 1 document of the plan's folder).
//
// The render thread draws a handed packet while the main thread goes on, and the packet names game
// objects that the draw reads. Before a game object is destroyed, or one of three bulk sites runs, the
// calling thread waits until the render thread read every world packet handed so far (the main
// thread's packets; the loading screen's thread hands its own, see game::WorldPacketPending), for at
// most kWaitLimit:
//   - the deferred destruction JADE_DEFERRED_DESTROY (end of frame, world streamer), at its start,
//   - the display's queued destroys JADE_DISPLAY_DESTROYS (in every display), at its start,
//   - the sector switch JADE_SECTOR_SWITCH (once per tick), at its first destroy or engine free,
//     since it runs every tick and almost always destroys nothing,
//   - any other JADE_DESTROY_OBJECT (scripts, effects, the lights' rebuild under the display).
// It does not wait when it holds the device lock (the draw takes it) or is the render thread; those
// calls, and the timeouts, run exposed.
//
// Counters, logged every kReportTicks end-of-frame calls when they changed: per site the calls with
// work, those that found a world packet not read, the waits and their time, timeouts, the destroys
// (JADE_DESTROY_OBJECT) and frees (JADE_HEAP_FREE; JADE_FREE for the engine's) inside, and those that
// ran exposed; for the other destroys, the callers that found a packet not read.
//
// ROAN_DESTRUCTION_WAITS=0 turns the waits off (the counters stay). The hooks run on whichever thread
// calls the engine functions.

#include <string>

namespace game::destruction
{
    // The counters, one line.
    std::string Summary();
}
