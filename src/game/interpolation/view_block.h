#pragma once

// The view block of the main step (82684518): view setup, display, effects draw and the delivery to
// the render thread, which the game runs once per main step after the logic.

#include <ppc_config.h>
#include <ppc_context.h>

#include <cstdint>

namespace game::interpolation
{
    // Which functions RunViewBlock calls.
    enum class ViewBlockCalls
    {
        // The originals of its five calls: hooks on those (engine_hooks.cpp) do not run, hooks on the
        // functions they call do. For the purity probe's replay, which the probes must not see.
        kOriginals,
        // The hooked symbols, as the main step calls them. For the iterations without a tick, which
        // the front's hooks treat like the main step's own view block.
        kHooked,
    };

    // Runs the view block as the main step does, under the same guards (a view exists and views are
    // not off). sectorSwitch is the delivery's flag; false keeps the sector switch (which can destroy
    // objects) out of it. Main thread, outside the main step's own view block. Returns whether the
    // block ran.
    bool RunViewBlock(PPCContext& ctx, uint8_t* base, bool sectorSwitch, ViewBlockCalls calls);

    // The state a view block outside the tick's own (between ticks, the purity probe's replay) must
    // leave as the tick left it. On construction it sets the time structure's dt (+56, +64, +68) to 0,
    // so what the display integrates by the dt does not advance again (the material animators, step 0
    // section 7), and saves the camera cut counter, which the display decrements per call and the
    // console decrements once per tick. Restore, or the end of the scope when Restore was not called,
    // puts both back. Main thread.
    class ExtraViewBlockState
    {
    public:
        ExtraViewBlockState();
        ~ExtraViewBlockState();
        ExtraViewBlockState(const ExtraViewBlockState&) = delete;
        ExtraViewBlockState& operator=(const ExtraViewBlockState&) = delete;

        void Restore();

    private:
        bool restored_ = false;
        uint32_t time_;
        uint64_t dt_ = 0;
        uint32_t dtScaled_ = 0, dtRaw_ = 0;
        uint32_t cutCounter_;
    };
}
