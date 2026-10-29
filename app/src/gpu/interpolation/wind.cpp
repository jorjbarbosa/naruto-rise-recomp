// The wind of the vegetation and of the grass between the ticks (state
// interpolation). Both are computed at each display, which runs in every
// frame, while the game's time moves only at the ticks (tick.h).
//
// Plants: the modifier ModifierVegetationWind applies at each display
// (8269EEC8): its phase, +72 of the modifier's data, is (the game's time, +48
// of the time structure, minus the start, +68) times the speed (+16, per 1/30
// s, times 30) plus a random offset (+64), and 8269E838 copies it, with the
// parameters, into the vertex constants of the plant
// (g_vVegetationWindParams[7]). Above 30 frames per second the plants stood
// still between two ticks and jumped at the next. While the copy runs inside a
// view block, the phase gets the image's advance past the tick
// (tick::ViewAhead) and comes back after it. The wind vector (8267D450) keeps
// the tick's: its samples step once per value of the game's time.
//
// Grass: the wave's time of a grass object, +312 (g_vWindParams[1].w of its
// patches), grows at each display by (a gust of the game's time + 1) times
// the object's strength (+36) times 0.01 (82599CD8, under the object's
// method 8259A050), a fixed step per frame: the wave ran twice as fast at 60
// frames per second, four times at 120. Inside a view block the strength is
// scaled by the frame's share of the tick (tick::ViewFactor) for the step and
// comes back after it; the patches drawn in the same call (82597780) read the
// time, not the strength.
#include "gpu/interpolation/tick.h"
#include "gpu/common/guest_memory.h"
#include <rex/hook.h>
#include <rex/ppc/context.h>

extern "C" void __imp__sub_8269E838(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82599CD8(PPCContext& __restrict, uint8_t*);

REX_HOOK_RAW(sub_8269E838) {
  const double ahead = gpu::tick::ViewAhead();
  const uint32_t data = ctx.r4.u32;
  if (ahead <= 0 || !data) {
    __imp__sub_8269E838(ctx, base);
    return;
  }
  const float phase = gpu::GetFloat(base, data + 72);
  gpu::PutFloat(base, data + 72, float(phase + ahead * double(gpu::GetFloat(base, data + 16)) * 30.0));
  __imp__sub_8269E838(ctx, base);
  gpu::PutFloat(base, data + 72, phase);
}

REX_HOOK_RAW(sub_82599CD8) {
  const double factor = gpu::tick::ViewFactor();
  const uint32_t grass = ctx.r3.u32;
  if (factor == 1.0 || !grass) {
    __imp__sub_82599CD8(ctx, base);
    return;
  }
  const float strength = gpu::GetFloat(base, grass + 36);
  gpu::PutFloat(base, grass + 36, float(strength * factor));
  __imp__sub_82599CD8(ctx, base);
  gpu::PutFloat(base, grass + 36, strength);
}
