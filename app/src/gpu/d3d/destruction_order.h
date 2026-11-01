#pragma once

// The order between the title's destructions and the render thread's drawing
// of the lists already delivered to it (NARUTORISE-7).
//
// The tick (82684518) hands its lists to the render thread (the delivery
// 8259FF40 -> 820E8290 -> 820EFFB8: Set 82C0F028, Wait 82C0F030) and then runs
// its deferred destruction (8267DDC0 -> 82659060: releases, deletes and frees
// the objects queued in dword_82C0EBF4) while the render thread draws those
// lists (820F02C0 -> 820FC008), which can still name the destroyed objects and
// their textures. The title waits for the drawing only at the next delivery
// (Wait 82C0F02C), so a late render thread reads freed memory. Two more
// destructions of the view block come before that wait: the display's
// deferred objects (82659F40 under 8265A058, up to 128 per frame) and the
// sector switch at the start of the delivery (8267DA90, which destroys and
// frees the objects of the sectors it leaves).
//
// Here each of the three, when it has work, first waits until the render
// thread has drawn every delivered list, at most kWaitMax. It does not wait
// when the thread holds the device lock (82C0F008), which the drawing takes.
// REX_NATIVE_DESTRUCTION_ORDER=0 turns the waits off; the counters stay (the
// calls, those in a frame without a tick, those that found lists not drawn,
// and the destroys (8266BA98) and frees (820C0018) inside each).
#include <cstdint>
#include <string>

namespace gpu::destruction_order {
// The render thread has drawn a delivered list (the end of 820FC008).
void ListsDrawn();
// The engine's free (820C0018), any thread: counted for the destruction it
// runs inside.
void NoteFree();
// The counters of the three destructions, as JSON fields.
std::string Summary();
}  // namespace gpu::destruction_order
