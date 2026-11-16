// The overrides of the Jade engine functions the state interpolation front observes or gates. The
// three loop functions ask the clock whether the iteration runs the logic; the others run the original
// and forward to the view clock and the probes (motion record, purity probe, data watch). With the
// clock and the probes off, the loop functions test their flags and the other hooks one thread-local
// flag before calling the original.

#include "game/addresses.h"
#include "game/interpolation/clock.h"
#include "game/interpolation/data_watch.h"
#include "game/interpolation/motion_record.h"
#include "game/interpolation/purity_probe.h"
#include "game/interpolation/view_clock.h"
#include "kernel/kernel.h"

DECLARE_ORIGINAL(JADE_HUD_MAP);
DECLARE_ORIGINAL(JADE_WORLD_CHANGE);
DECLARE_ORIGINAL(JADE_MAIN_STEP);
DECLARE_ORIGINAL(JADE_VIEW_SETUP);
DECLARE_ORIGINAL(JADE_DISPLAY);
DECLARE_ORIGINAL(JADE_DELIVER);
DECLARE_ORIGINAL(JADE_RENDER_OBJECT);
DECLARE_ORIGINAL(JADE_BONE_COPY);

namespace
{
    namespace interpolation = game::interpolation;

    // Set on the thread that runs the main step, between its entry and its exit, when the clock or a
    // probe is on; also in an iteration without a tick, whose view block the hooks treat like the main
    // step's. The engine also displays and delivers outside the main step (world loading, the loading
    // screen's thread); the view clock and the probes only act inside it.
    thread_local bool t_inMainStep = false;
    uint64_t g_iteration = 0; // main thread
}

// The HUD map (82677D20), the main loop's first call. Hooked so an iteration without a tick skips it.
PPC_FUNC(JADE_HUD_MAP)
{
    if (interpolation::clock::Enabled() && !interpolation::clock::LogicRuns())
        return;
    ORIGINAL(JADE_HUD_MAP)(ctx, base);
}

// The pending world change (826843D8), the main loop's second call. Hooked so an iteration without a
// tick skips it: a world change belongs to the logic.
PPC_FUNC(JADE_WORLD_CHANGE)
{
    if (interpolation::clock::Enabled() && !interpolation::clock::LogicRuns())
        return;
    ORIGINAL(JADE_WORLD_CHANGE)(ctx, base);
}

// The main step (82684518): one iteration of the main loop, the logic, the view block and the end of
// frame. Hooked so an iteration without a tick runs only the view block (clock.h), and to bracket it
// for the probes; the purity probe compares and replays after it.
PPC_FUNC(JADE_MAIN_STEP)
{
    bool gated = interpolation::clock::Enabled();
    bool motion = interpolation::motion::Enabled();
    bool purity = interpolation::purity::Enabled();
    bool watch = interpolation::watch::Enabled();
    if (!gated && !motion && !purity && !watch)
    {
        ORIGINAL(JADE_MAIN_STEP)(ctx, base);
        return;
    }
    bool logic = !gated || interpolation::clock::LogicRuns();
    g_iteration++;
    if (motion)
        interpolation::motion::MainStepStart(g_iteration, logic, interpolation::clock::Current().alpha);
    if (purity)
        interpolation::purity::MainStepStart();
    if (watch)
        interpolation::watch::MainStepStart();
    t_inMainStep = true;
    if (logic)
        ORIGINAL(JADE_MAIN_STEP)(ctx, base);
    else
        interpolation::clock::RunBetweenTicks(ctx, base);
    t_inMainStep = false;
    if (gated)
        interpolation::view_clock::ViewBlockEnd();
    if (motion)
        interpolation::motion::MainStepEnd();
    if (purity)
        interpolation::purity::MainStepEnd(ctx, base);
    if (watch)
        interpolation::watch::MainStepEnd();
    if (gated)
        interpolation::clock::IterationEnd();
}

// View setup (82684050), the first call of the view block. Hooked so the view clock gives the block the
// image's dt, and to mark the block's start for the probes (after the view clock's stores, which stay
// out of the purity probe's window).
PPC_FUNC(JADE_VIEW_SETUP)
{
    if (t_inMainStep && interpolation::clock::Enabled())
        interpolation::view_clock::ViewBlockStart();
    if (t_inMainStep && interpolation::purity::Enabled())
        interpolation::purity::ViewBlockStart();
    if (t_inMainStep && interpolation::watch::Enabled())
        interpolation::watch::ViewBlockStart();
    ORIGINAL(JADE_VIEW_SETUP)(ctx, base);
}

// The display (8265A058, r3 world, r4 view). Hooked so the motion record reads the frame around it,
// and so the purity probe names the rendered objects while they are alive: the delivery that follows
// can switch sectors and destroy objects.
PPC_FUNC(JADE_DISPLAY)
{
    bool record = t_inMainStep && interpolation::motion::Enabled();
    if (record)
        interpolation::motion::DisplayStart(ctx.r4.u32);
    ORIGINAL(JADE_DISPLAY)(ctx, base);
    if (record)
        interpolation::motion::DisplayEnd();
    if (t_inMainStep && interpolation::purity::Enabled())
        interpolation::purity::DisplayEnd();
}

// The delivery (8259FF40, r3 view, r4 sector switch), the last call of the view block. Hooked to mark
// the block's end for the probes, then to put the tick's dt back.
PPC_FUNC(JADE_DELIVER)
{
    uint32_t view = ctx.r3.u32;
    ORIGINAL(JADE_DELIVER)(ctx, base);
    if (t_inMainStep && interpolation::purity::Enabled())
        interpolation::purity::ViewBlockEnd();
    if (t_inMainStep && interpolation::watch::Enabled())
        interpolation::watch::ViewBlockEnd(view);
    if (t_inMainStep && interpolation::clock::Enabled())
        interpolation::view_clock::ViewBlockEnd();
}

// Render of one object by the display (825E28D0, WOR_Render_One_GO, r6 the object). Hooked so the
// probes know the objects of the frame.
PPC_FUNC(JADE_RENDER_OBJECT)
{
    if (t_inMainStep)
    {
        uint32_t object = ctx.r6.u32;
        if (interpolation::motion::Enabled())
            interpolation::motion::NoteRendered(object);
        if (interpolation::purity::Enabled())
            interpolation::purity::NoteRendered(object);
    }
    ORIGINAL(JADE_RENDER_OBJECT)(ctx, base);
}

// Copy of a bone matrix into the frame's packet (820E91E0, r3 the bone, r4 its matrix relative to the
// object), reached through the display interface. Hooked so the motion record follows bones.
PPC_FUNC(JADE_BONE_COPY)
{
    if (t_inMainStep && interpolation::motion::Enabled())
        interpolation::motion::NoteBone(ctx.r3.u32, ctx.r4.u32);
    ORIGINAL(JADE_BONE_COPY)(ctx, base);
}
