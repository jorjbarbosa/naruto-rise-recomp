// The view block of the main step, run outside it (view_block.h).

#include "game/interpolation/view_block.h"

#include "game/addresses.h"
#include "memory.h"

DECLARE_ORIGINAL(JADE_VIEW_SETUP);
DECLARE_ORIGINAL(JADE_DISPLAY);
DECLARE_ORIGINAL(JADE_DELIVER);
DECLARE_ORIGINAL(JADE_VIEW_PREPARE);
DECLARE_ORIGINAL(JADE_EFFECTS_DRAW);

namespace game::interpolation
{
    bool RunViewBlock(PPCContext& ctx, uint8_t* base, bool sectorSwitch)
    {
        uint32_t view = mem::Load32(jade::kView);
        if (view == 0 || mem::Load32(jade::kViewsOff) != 0)
            return false;

        // The order of the main step (82684518).
        ctx.r3.u64 = view;
        ORIGINAL(JADE_VIEW_SETUP)(ctx, base);
        ctx.r3.u64 = view;
        ORIGINAL(JADE_VIEW_PREPARE)(ctx, base);
        uint32_t world = mem::Load32(jade::kWorld);
        mem::Store32(view + jade::view::kWorld, world);
        if (world != 0)
        {
            mem::Store32(mem::Load32(world + jade::world::kDisplay) + jade::world::kDisplayView, view);
            ctx.r3.u64 = world;
            ctx.r4.u64 = view;
            ORIGINAL(JADE_DISPLAY)(ctx, base);
        }
        ctx.r3.u64 = mem::Load32(jade::kEffects);
        ctx.r4.u64 = view;
        ORIGINAL(JADE_EFFECTS_DRAW)(ctx, base);
        ctx.r3.u64 = view;
        ctx.r4.u64 = sectorSwitch ? 1 : 0;
        ORIGINAL(JADE_DELIVER)(ctx, base);
        return true;
    }
}
