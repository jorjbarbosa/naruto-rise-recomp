// The hooks that give the display's readers of the game time the view clock's image time.
//
// Plan section 8.5, way 2: one hook per consumer (step 3 document, section 4). Each hook brackets the
// original with a view_clock::TimeScope, which does nothing outside a view block under the gate, so
// with the clock off they cost one test. Consumers left on the tick's time are listed in the step
// document with why.

#include "game/addresses.h"
#include "game/interpolation/view_clock.h"
#include "game/interpolation/world.h"
#include "memory.h"

#include <ppc_config.h>
#include <ppc_context.h>

#include <bit>

DECLARE_ORIGINAL(JADE_MODIFIER_1_APPLY);
DECLARE_ORIGINAL(JADE_MODIFIER_26_APPLY);
DECLARE_ORIGINAL(JADE_MODIFIER_37_STEP);
DECLARE_ORIGINAL(JADE_MODIFIER_42_ANGLE);
DECLARE_ORIGINAL(JADE_MODIFIER_47_TIME);
DECLARE_ORIGINAL(JADE_MODIFIER_59_APPLY);
DECLARE_ORIGINAL(JADE_WIND_FIELD);
DECLARE_ORIGINAL(JADE_GRASS_WAVES);
DECLARE_ORIGINAL(JADE_GRASS_GUST);
DECLARE_ORIGINAL(JADE_GRASS_SCROLL);
DECLARE_ORIGINAL(JADE_SCREEN_EFFECT_FADE);
DECLARE_ORIGINAL(JADE_SCREEN_EFFECT_SCROLL);
DECLARE_ORIGINAL(JADE_SCREEN_EFFECT_PROGRESS);
DECLARE_ORIGINAL(JADE_TECHNIQUE_WAVE);

namespace
{
    namespace view_clock = game::interpolation::view_clock;
    using view_clock::Time;
    using view_clock::TimeScope;
}

// Phases of the display. Each is a function of the game time (minus the lock accumulator) that the
// display draws from; the state they keep is a cache of it, or a latch that stays consistent with a
// monotonic time (modifier 1's fade restarts at the time it sees). The parts that accumulate do it by
// the dt (+64), which the view clock already scales.

// Modifier 1 (OnduleTonCorps) Apply: amplitudes and a phase from the game time, and its fade timer
// (8260B0E0, called only from here).
PPC_FUNC(JADE_MODIFIER_1_APPLY)
{
    TimeScope scope(Time::kImage);
    ORIGINAL(JADE_MODIFIER_1_APPLY)(ctx, base);
}

// Modifier 26 Apply: sets the object's scale from sines of the game time (Unapply restores it).
PPC_FUNC(JADE_MODIFIER_26_APPLY)
{
    TimeScope scope(Time::kImage);
    ORIGINAL(JADE_MODIFIER_26_APPLY)(ctx, base);
}

// Modifier 37's step: sines of the game time, and the wrap of the values it accumulates by the dt.
PPC_FUNC(JADE_MODIFIER_37_STEP)
{
    TimeScope scope(Time::kImage);
    ORIGINAL(JADE_MODIFIER_37_STEP)(ctx, base);
}

// The angle of modifiers 42 and 45: from the game time in their timed mode (otherwise it accumulates
// the dt it is given and does not read the time).
PPC_FUNC(JADE_MODIFIER_42_ANGLE)
{
    TimeScope scope(Time::kImage);
    ORIGINAL(JADE_MODIFIER_42_ANGLE)(ctx, base);
}

// Modifier 47: hands the game time to a shader parameter.
PPC_FUNC(JADE_MODIFIER_47_TIME)
{
    TimeScope scope(Time::kImage);
    ORIGINAL(JADE_MODIFIER_47_TIME)(ctx, base);
}

// Modifier 59 Apply, vegetation wind: a phase from the game time. It calls the wind field, which gets
// the tick's time back (below).
PPC_FUNC(JADE_MODIFIER_59_APPLY)
{
    TimeScope scope(Time::kImage);
    ORIGINAL(JADE_MODIFIER_59_APPLY)(ctx, base);
}

// The wind field: once per distinct game time it moves its cursor to the next wind source and tests it
// against the object (a raycast). The step is per value, not by the time's advance: fed the image time,
// it would scan several sources per tick, with as many raycasts. It keeps the tick's time; this matters
// only inside modifier 59's scope (types 18, 19 and 22 call it without one).
PPC_FUNC(JADE_WIND_FIELD)
{
    TimeScope scope(Time::kTick);
    ORIGINAL(JADE_WIND_FIELD)(ctx, base);
}

// The grass' wave cosines and gust.
PPC_FUNC(JADE_GRASS_WAVES)
{
    TimeScope scope(Time::kImage);
    ORIGINAL(JADE_GRASS_WAVES)(ctx, base);
}

PPC_FUNC(JADE_GRASS_GUST)
{
    TimeScope scope(Time::kImage);
    ORIGINAL(JADE_GRASS_GUST)(ctx, base);
}

// The grass scroll: each call adds (sin(0.1 x time) + 1) x speed x 0.01, with no dt, so at the game's
// rate it steps once per display, one display per tick. Above it the step is scaled by the image's
// advance (plan section 8.5: a consumer that steps per display gets the frame's share of the tick),
// with the step's own phase at the image time.
PPC_FUNC(JADE_GRASS_SCROLL)
{
    uint32_t scroll = ctx.r3.u32 + game::jade::grass::kScroll;
    float before = game::interpolation::LoadFloat(scroll);
    TimeScope scope(Time::kImage);
    ORIGINAL(JADE_GRASS_SCROLL)(ctx, base);
    if (scope.Writes())
    {
        float after = game::interpolation::LoadFloat(scroll);
        float scaled = before + float(double(after - before) * view_clock::Advance());
        mem::Store32(scroll, std::bit_cast<uint32_t>(scaled));
    }
}

// A technique's texture wave: a sine of the game time and the position. Reached from the draw code's
// technique table; in the village it runs on the main thread during the display (most calls nested in
// another consumer's scope, a fifth outside any), so it is a phase of the display like the others. The
// draw code also runs on the render thread: there the image scope does nothing and the exclusion keeps
// it on the tick's time (each scope acts only on its own thread).
PPC_FUNC(JADE_TECHNIQUE_WAVE)
{
    view_clock::RenderReadScope exclusion;
    TimeScope scope(Time::kImage);
    ORIGINAL(JADE_TECHNIQUE_WAVE)(ctx, base);
}

// Render thread readers: the screen effects (fade, scroll, progress), vtable slot +20 of their classes
// called from the packet's draw (820F2470), read the time structure live while the main thread
// displays the next frame. Each one excludes the main thread's TimeScopes for its call, so it reads the
// tick's time as before (view_clock.h).
PPC_FUNC(JADE_SCREEN_EFFECT_FADE)
{
    view_clock::RenderReadScope exclusion;
    ORIGINAL(JADE_SCREEN_EFFECT_FADE)(ctx, base);
}

PPC_FUNC(JADE_SCREEN_EFFECT_SCROLL)
{
    view_clock::RenderReadScope exclusion;
    ORIGINAL(JADE_SCREEN_EFFECT_SCROLL)(ctx, base);
}

PPC_FUNC(JADE_SCREEN_EFFECT_PROGRESS)
{
    view_clock::RenderReadScope exclusion;
    ORIGINAL(JADE_SCREEN_EFFECT_PROGRESS)(ctx, base);
}
