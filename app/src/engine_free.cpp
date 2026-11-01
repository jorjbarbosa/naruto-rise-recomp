// narutorise - ReXGlue Recompiled Project
//
// The engine's free (sub_820C0018: the kind in r3, the memory in r4, the heap
// in r5), seen by the units that follow a guest object until it is freed:
// the save thumbnail (save_thumbnail.h) and, with the native renderer, the
// tracks of the state interpolation, the ring of the crash dump's last frees
// (which records only when the dump is on) and the counters of the title's
// destructions (destruction_order.h).

#include <cstdint>

#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/system/thread_state.h>

#include "save_thumbnail.h"
#ifdef NARUTORISE_NATIVE_RENDERER_BOOTSTRAP
#include "gpu/d3d/destruction_order.h"
#include "gpu/diagnostics/crash_dump.h"
#include "gpu/interpolation/transforms.h"
#endif

extern "C" void __imp__sub_820C0018(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_820C0018) {
#ifdef NARUTORISE_NATIVE_RENDERER_BOOTSTRAP
  auto* state = rex::runtime::ThreadState::Get();
  gpu::crash_dump::NoteFree(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.lr, state ? state->thread_id() : 0);
  gpu::interpolation::NoteFree(ctx.r4.u32);
  gpu::destruction_order::NoteFree();
#endif
  narutorise::save_thumbnail::NoteFree(ctx.r4.u32);
  __imp__sub_820C0018(ctx, base);
}
