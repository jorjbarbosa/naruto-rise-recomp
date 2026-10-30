#pragma once

// The order between the tick's deferred destruction and the render thread's
// drawing of the lists the tick has just delivered (NARUTORISE-7).
//
// The tick (82684518) hands its lists to the render thread (the delivery
// 8259FF40 -> 820E8290 -> 820EFFB8: Set 82C0F028, Wait 82C0F030) and then runs
// its deferred destruction (8267DDC0 -> 82659060: releases, deletes and frees
// the objects queued in dword_82C0EBF4) while the render thread draws those
// lists (820F02C0 -> 820FC008), which can still name the destroyed objects and
// their textures. The title waits for the drawing only at the next delivery
// (Wait 82C0F02C), so a late render thread reads freed memory. Here a
// destruction with objects queued first waits until the render thread has
// drawn every delivered list, at most kWaitMax. It does not wait when the
// thread holds the device lock (82C0F008), which the drawing takes.
// REX_NATIVE_DESTRUCTION_ORDER=0 turns the wait off.
#include <cstdint>

namespace gpu::destruction_order {
// The render thread has drawn a delivered list (the end of 820FC008).
void ListsDrawn();
}  // namespace gpu::destruction_order
