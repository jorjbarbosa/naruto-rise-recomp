// The view block of the main step, run outside it (view_block.h).

#include "game/interpolation/view_block.h"

#include "game/addresses.h"
#include "game/interpolation/world.h"
#include "memory.h"

DECLARE_ORIGINAL(JADE_VIEW_SETUP);
DECLARE_ORIGINAL(JADE_DISPLAY);
DECLARE_ORIGINAL(JADE_DELIVER);
DECLARE_ORIGINAL(JADE_VIEW_PREPARE);
DECLARE_ORIGINAL(JADE_EFFECTS_DRAW);
PPC_EXTERN_FUNC(JADE_VIEW_SETUP);
PPC_EXTERN_FUNC(JADE_DISPLAY);
PPC_EXTERN_FUNC(JADE_DELIVER);
PPC_EXTERN_FUNC(JADE_VIEW_PREPARE);
PPC_EXTERN_FUNC(JADE_EFFECTS_DRAW);

namespace game::interpolation
{
    namespace
    {
        using GuestFunction = void (*)(PPCContext& __restrict, uint8_t*);

        struct Calls
        {
            GuestFunction setup, prepare, display, effects, deliver;
        };

        constexpr Calls kOriginalCalls = { ORIGINAL(JADE_VIEW_SETUP), ORIGINAL(JADE_VIEW_PREPARE), ORIGINAL(JADE_DISPLAY),
            ORIGINAL(JADE_EFFECTS_DRAW), ORIGINAL(JADE_DELIVER) };
        constexpr Calls kHookedCalls = { JADE_VIEW_SETUP, JADE_VIEW_PREPARE, JADE_DISPLAY, JADE_EFFECTS_DRAW, JADE_DELIVER };
    }

    bool RunViewBlock(PPCContext& ctx, uint8_t* base, bool sectorSwitch, ViewBlockCalls calls)
    {
        uint32_t view = mem::Load32(jade::kView);
        if (view == 0 || mem::Load32(jade::kViewsOff) != 0)
            return false;
        const Calls& call = calls == ViewBlockCalls::kHooked ? kHookedCalls : kOriginalCalls;

        // The order of the main step (82684518).
        ctx.r3.u64 = view;
        call.setup(ctx, base);
        ctx.r3.u64 = view;
        call.prepare(ctx, base);
        uint32_t world = mem::Load32(jade::kWorld);
        mem::Store32(view + jade::view::kWorld, world);
        if (world != 0)
        {
            mem::Store32(mem::Load32(world + jade::world::kDisplay) + jade::world::kDisplayView, view);
            ctx.r3.u64 = world;
            ctx.r4.u64 = view;
            call.display(ctx, base);
        }
        ctx.r3.u64 = mem::Load32(jade::kEffects);
        ctx.r4.u64 = view;
        call.effects(ctx, base);
        ctx.r3.u64 = view;
        ctx.r4.u64 = sectorSwitch ? 1 : 0;
        call.deliver(ctx, base);
        return true;
    }

    ExtraViewBlockState::ExtraViewBlockState(ExtraDt dt)
        : time_(dt == ExtraDt::kZero ? TimeStructure() : 0), cutCounter_(mem::Load32(jade::kCutCounter)),
          cutFadeRate_(mem::Load32(jade::kCutFadeRate)), cutFlag_(mem::Load32(jade::kCutFlag))
    {
        for (uint32_t i = 0; i < cutDetector_.size(); i++)
            cutDetector_[i] = mem::Load32(jade::kCutDetector + jade::kCutDetectorState + 4 * i);
        if (time_ == 0)
            return;
        dt_ = mem::Load64(time_ + jade::tim::kDt);
        dtScaled_ = mem::Load32(time_ + jade::tim::kDtScaled);
        dtRaw_ = mem::Load32(time_ + jade::tim::kDtRaw);
        mem::Store64(time_ + jade::tim::kDt, 0);
        mem::Store32(time_ + jade::tim::kDtScaled, 0);
        mem::Store32(time_ + jade::tim::kDtRaw, 0);
    }

    ExtraViewBlockState::~ExtraViewBlockState()
    {
        Restore();
    }

    void ExtraViewBlockState::Restore()
    {
        if (restored_)
            return;
        restored_ = true;
        mem::Store32(jade::kCutCounter, cutCounter_);
        mem::Store32(jade::kCutFadeRate, cutFadeRate_);
        mem::Store32(jade::kCutFlag, cutFlag_);
        for (uint32_t i = 0; i < cutDetector_.size(); i++)
            mem::Store32(jade::kCutDetector + jade::kCutDetectorState + 4 * i, cutDetector_[i]);
        if (time_ == 0)
            return;
        mem::Store64(time_ + jade::tim::kDt, dt_);
        mem::Store32(time_ + jade::tim::kDtScaled, dtScaled_);
        mem::Store32(time_ + jade::tim::kDtRaw, dtRaw_);
    }
}
