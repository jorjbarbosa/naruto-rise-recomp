#pragma once

// The tick clock and the gate (state interpolation plan, sections 8.3 and 8.4; step 2 document of the
// plan's folder): the game is shown at a frame rate above its own while its logic keeps the title's
// fixed step.
//
// The main loop (82684920) calls the HUD map, the pending world change and the main step once per
// iteration. Under the title's fixed step (time structure +72, period +76: 2/60 s in the open world,
// 1/60 s in fights) the clock runs on the timeline of the presented frames: each iteration is one
// frame of 1 / rate s, and it is a tick (the three calls run as the original) when a period of image
// time is due since the last tick. An iteration without a tick runs only the view block, with the
// delivery's flag 0 (the sector switch stays with the tick) and the camera cut counter put back after
// it (it counts ticks as on the console; view_block.h, ExtraViewBlockState). The dt of every view
// block, the tick's and the others, is the image's advance since the previous one (view_clock.h).
//
// The gate decides only under the fixed step, with a period of at least 1.25 frames. Otherwise every
// iteration is a tick: under the fixed step the limiter then paces at the tick rate (1 / period), so
// the game keeps its speed; with a measured dt (menus) it keeps the title's presentation interval.
//
// ROAN_FRAME_RATE=<31..240> turns it on at that rate; unset, or 30 and below, it is off and the game
// runs at its own rate (the hooks test one flag). Main thread only: the calls come from
// engine_hooks.cpp, from the three loop functions.

#include <ppc_config.h>
#include <ppc_context.h>

#include <cstdint>

namespace game::interpolation::clock
{
    // Whether the clock is on (read once).
    bool Enabled();

    // The current loop iteration.
    struct Frame
    {
        bool gate = false;   // the gate decides (fixed step, a period of at least 1.25 frames)
        bool tick = true;    // the logic runs in this iteration
        float period = 0.0f; // the tick period (+76) while the gate decides, else 0
        float alpha = 0.0f;  // image time past the last tick, in periods, 0 to 1; 0 without the gate
        // While the gate decides: the timeline, which changes when it restarts (a change of the gate,
        // the period or the pace), and the ticks taken since it started. The view clock takes the
        // image's advance from them and alpha.
        uint32_t timeline = 0;
        uint64_t ticks = 0;
    };
    const Frame& Current();

    // Called first by each of the three loop functions; the first call of an iteration decides it.
    // Returns whether the logic runs in this iteration: when false the HUD map and the world change are
    // skipped and the main step runs RunBetweenTicks instead of the original.
    bool LogicRuns();

    // The main step of an iteration without a tick: the view block (with the hooked functions) as
    // described above. When there is no view block to run (no view, views off) it sleeps until the
    // next frame's slot instead, so the loop does not spin.
    void RunBetweenTicks(PPCContext& ctx, uint8_t* base);

    // The iteration ended (the main step's exit).
    void IterationEnd();
}
