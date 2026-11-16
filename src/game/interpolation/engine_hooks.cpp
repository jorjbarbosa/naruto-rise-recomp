// The overrides of the Jade engine functions the state interpolation front observes. Each one runs
// the original and forwards to the probes (motion record, purity probe). With the probes off, the
// main step tests two flags and the other hooks one thread-local flag before calling the original.

#include "game/addresses.h"
#include "game/interpolation/motion_record.h"
#include "game/interpolation/purity_probe.h"
#include "kernel/kernel.h"

DECLARE_ORIGINAL(JADE_MAIN_STEP);
DECLARE_ORIGINAL(JADE_VIEW_SETUP);
DECLARE_ORIGINAL(JADE_DISPLAY);
DECLARE_ORIGINAL(JADE_DELIVER);
DECLARE_ORIGINAL(JADE_RENDER_OBJECT);
DECLARE_ORIGINAL(JADE_BONE_COPY);

namespace
{
    namespace interpolation = game::interpolation;

    // Set on the thread that runs the main step, between its entry and its exit, when a probe is on.
    // The engine also displays and delivers outside the main step (world loading, the loading
    // screen's thread); the probes only look inside it.
    thread_local bool t_inMainStep = false;
    uint64_t g_iteration = 0; // main thread
}

// The main step (82684518): one iteration of the main loop, the logic, the view block and the end of
// frame. Hooked to bracket it for the probes; the purity probe compares and replays after it.
PPC_FUNC(JADE_MAIN_STEP)
{
    bool motion = interpolation::motion::Enabled();
    bool purity = interpolation::purity::Enabled();
    if (!motion && !purity)
    {
        ORIGINAL(JADE_MAIN_STEP)(ctx, base);
        return;
    }
    g_iteration++;
    if (motion)
        interpolation::motion::MainStepStart(g_iteration);
    if (purity)
        interpolation::purity::MainStepStart();
    t_inMainStep = true;
    ORIGINAL(JADE_MAIN_STEP)(ctx, base);
    t_inMainStep = false;
    if (motion)
        interpolation::motion::MainStepEnd();
    if (purity)
        interpolation::purity::MainStepEnd(ctx, base);
}

// View setup (82684050), the first call of the view block. Hooked to mark the block's start for the
// purity probe.
PPC_FUNC(JADE_VIEW_SETUP)
{
    if (t_inMainStep && interpolation::purity::Enabled())
        interpolation::purity::ViewBlockStart();
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
// the block's end for the purity probe.
PPC_FUNC(JADE_DELIVER)
{
    ORIGINAL(JADE_DELIVER)(ctx, base);
    if (t_inMainStep && interpolation::purity::Enabled())
        interpolation::purity::ViewBlockEnd();
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
