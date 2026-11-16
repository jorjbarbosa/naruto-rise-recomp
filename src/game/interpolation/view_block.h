#pragma once

// The view block of the main step (82684518): view setup, display, effects draw and the delivery to
// the render thread, which the game runs once per main step after the logic.

#include <ppc_config.h>
#include <ppc_context.h>

#include <cstdint>

namespace game::interpolation
{
    // Runs the view block as the main step does, under the same guards (a view exists and views are
    // not off), with the originals of its five calls: hooks on those (engine_hooks.cpp) do not run,
    // hooks on the functions they call do. sectorSwitch is the delivery's flag; false keeps the
    // sector switch (which can destroy objects) out of it. Main thread, outside the main step's own
    // view block. Returns whether the block ran.
    bool RunViewBlock(PPCContext& ctx, uint8_t* base, bool sectorSwitch);
}
