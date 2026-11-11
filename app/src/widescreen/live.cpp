// The screen changed during the session. The window, the monitor or the
// option asks for another logical screen; the change runs in three steps at
// points where the game itself is between frames:
//   A. the camera and the layout (entry of 8259E908, the view block of the
//      main thread), once the window kept its size for a moment: the next
//      list is built for the new screen;
//   B. the game's targets (after 820ED9D8, inside the frame delivery: the
//      render thread finished the previous list, Swap included, and waits;
//      the delivering thread holds the device lock, and the game itself
//      destroys GPU resources there): unbind, release and create again
//      +504, +516, +520 and +524 in the order of 820E6B00, bind again, write
//      S and its copies, queue the old post-process pool and build it again
//      with the game's own function (8210BA38, which also resets the 44
//      effects), the viewport and the 16/9 of the effect;
//   C. the native side of the deferred destruction (820F2C70, two deliveries
//      later) of the pool entries step B queued: the entry's embedded texture
//      header and the surfaces of its aliases, which the game never releases.
// One change at a time: the next waits for the deferred destruction of the
// previous one. When the game's creation refuses a target (a null header),
// the targets return to the previous screen and that screen is not asked
// again; a failure of the engine inside a creation stops the game in order,
// as anywhere in the renderer (the screen never asks for a surface the engine
// does not admit: LogicalSize).
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

#include <rex/hook.h>
#include <rex/logging.h>

#include "gpu/common/guest_memory.h"
#include "gpu/d3d/d3d.h"
#include "gpu/d3d/hooks.h"
#include "gpu/d3d/resources.h"
#include "widescreen/widescreen.h"

extern "C" void __imp__sub_8259E908(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_820ED9D8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_820F2C70(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_820E76A0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_820EE9C8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_820F2A08(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8210BA38(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8212DD80(PPCContext& __restrict, uint8_t*);
// Through the native hooks: binding and creation are tracked by the renderer.
extern "C" void sub_8212ADA0(PPCContext& __restrict, uint8_t*);
extern "C" void sub_8212B108(PPCContext& __restrict, uint8_t*);
extern "C" void sub_8212D210(PPCContext& __restrict, uint8_t*);
extern "C" void sub_8212D330(PPCContext& __restrict, uint8_t*);

using namespace widescreen;
using gpu::Get32;
using gpu::Put32;

namespace {
constexpr uint32_t kRenderState = 0x82C170D8;
constexpr uint32_t kDeferredQueue = 0x82C19210;
constexpr uint32_t kColorFormat = 0x18280186, kDepthFormat = 0x1A220197, kFrontFormat = 0x28280106;
constexpr uint32_t kPoolEntries = 31;
// The deferred destruction runs two deliveries after the queue; the next
// change waits for it (every entry step B queued came back through step C),
// or for kSettleLimit deliveries if the game kept some entry.
constexpr uint32_t kSettleDeliveries = 3, kSettleLimit = 120;

std::atomic<uint64_t> pending{0};    // Packed screen of step A, waiting for step B; 0 = none.
std::atomic<uint64_t> refused{0};    // A screen whose targets could not be created.
std::atomic<uint32_t> since_change{kSettleLimit};
// The pool entries step B queued for the deferred destruction: step C acts
// on these alone, never on an entry the game destroys on its own.
std::mutex queued_mutex;
std::unordered_set<uint32_t> queued_entries;

// True when the previous change is over (see kSettleDeliveries).
bool PreviousChangeSettled() {
  const uint32_t count = since_change.load(std::memory_order_relaxed);
  if (count < kSettleDeliveries) return false;
  size_t queued = 0;
  {
    std::lock_guard lock(queued_mutex);
    queued = queued_entries.size();
  }
  if (!queued) return true;
  if (count < kSettleLimit) return false;
  static std::atomic<bool> recorded{false};
  if (!recorded.exchange(true))
    REXLOG_WARN("[narutorise] widescreen: {} pool entries of the previous change never came back; going on", queued);
  return true;
}

constexpr auto& Call = gpu::CallGuestArgs;

struct Targets {
  uint32_t front = 0, half_depth = 0, color = 0, depth = 0;
  bool Complete() const { return front && half_depth && color && depth; }
};

// The targets of 820E6B00 at a screen, in its order, with the EDRAM bases of
// the session's start (MainDepthEdramBase, HalfDepthEdramBase). `scratch`
// holds the 12-byte pParameters of D330.
Targets CreateTargets(PPCContext& ctx, uint8_t* base, uint32_t frame, uint32_t scratch, Size s) {
  Targets t;
  t.front = Call(ctx, base, sub_8212D210, frame, {s.width, s.height, 1, 1, 0, kFrontFormat, 0, 3});
  const uint32_t color_tiles = Call(ctx, base, __imp__sub_820E76A0, frame, {s.width / 2, s.height / 2, kColorFormat, 2});
  const auto parameters = [&](uint32_t edram) {
    Put32(base, scratch, edram);
    Put32(base, scratch + 4, 0);
    Put32(base, scratch + 8, 0);
    return scratch;
  };
  t.half_depth = Call(ctx, base, sub_8212D330, frame,
                      {s.width / 2, s.height / 2, kDepthFormat, 2, parameters(HalfDepthEdramBase(s, color_tiles))});
  t.color = Call(ctx, base, sub_8212D330, frame, {s.width, s.height, kColorFormat, 0, parameters(0)});
  t.depth = Call(ctx, base, sub_8212D330, frame, {s.width, s.height, kDepthFormat, 0, parameters(MainDepthEdramBase(s))});
  return t;
}

void Release(PPCContext& ctx, uint8_t* base, uint32_t frame, uint32_t resource) {
  if (resource) Call(ctx, base, __imp__sub_8212DD80, frame, {resource});
}

enum class Outcome {
  kApplied,   // The targets at the new screen.
  kRefused,   // The game refused a target: back at the previous screen.
  kNotReady,  // No device or S yet: nothing changed, step A asks again.
};

// Step B: the game's targets, S and the pool at screen `s`.
Outcome Recreate(PPCContext& ctx, uint8_t* base, Size s) {
  const Size previous = Current();
  const uint32_t owner = Get32(base, kDeviceOwner);
  const uint32_t device = owner ? Get32(base, owner + 16) : 0;
  const uint32_t screen = owner ? Get32(base, owner + 536) : 0;
  if (!device || !screen) {
    // Step A moved the camera and the layout already: back with the targets.
    WriteCamera(base, previous);
    SetLayout(previous);
    static std::atomic<bool> recorded{false};
    if (!recorded.exchange(true))
      REXLOG_WARN("[narutorise] widescreen resize {}x{} -> {}x{}: no device yet (owner {:08X}, device {:08X}, S {:08X})",
                  previous.width, previous.height, s.width, s.height, owner, device, screen);
    return Outcome::kNotReady;
  }
  const uint32_t frame = (ctx.r1.u32 - 2048) & ~15u, scratch = frame + 1024;
  // Unbind, as 820E6A68 does before its releases.
  Call(ctx, base, sub_8212ADA0, frame, {device, 0, 0});
  Call(ctx, base, sub_8212B108, frame, {device, 0});
  for (uint32_t field : {516u, 520u, 524u, 504u}) {
    Release(ctx, base, frame, Get32(base, owner + field));
    Put32(base, owner + field, 0);
  }
  SetCurrent(s);  // The native hooks size the targets, and BeginTiling its tiles, by it.
  Targets t = CreateTargets(ctx, base, frame, scratch, s);
  const bool created = t.Complete();
  if (!created) {
    REXLOG_ERROR("[narutorise] widescreen resize {}x{} -> {}x{}: target creation failed (front {:08X}, half depth {:08X}, "
                 "color {:08X}, depth {:08X}); back to {}x{}", previous.width, previous.height, s.width, s.height, t.front,
                 t.half_depth, t.color, t.depth, previous.width, previous.height);
    for (uint32_t resource : {t.front, t.half_depth, t.color, t.depth}) Release(ctx, base, frame, resource);
    s = previous;
    SetCurrent(s);
    t = CreateTargets(ctx, base, frame, scratch, s);
    // The game cannot draw without its targets, nor go on with null ones.
    if (!t.Complete()) {
      REXLOG_ERROR("[narutorise] widescreen resize: the targets at {}x{} failed again (front {:08X}, half depth {:08X}, "
                   "color {:08X}, depth {:08X})", s.width, s.height, t.front, t.half_depth, t.color, t.depth);
      gpu::StopInOrder(ctx, "widescreen-resize-targets-unproved", 88);
    }
  }
  Put32(base, owner + 504, t.front);
  Put32(base, owner + 524, t.half_depth);
  Put32(base, owner + 516, t.color);
  Put32(base, owner + 520, t.depth);
  Call(ctx, base, sub_8212ADA0, frame, {device, 0, t.color});
  Call(ctx, base, sub_8212B108, frame, {device, t.depth});
  // S and the copies it is computed from (820E81B0), then the pool: the old
  // entries to the game's deferred destruction, the new ones from S (the
  // bases before their aliases; vt+24 on the effects).
  Put32(base, owner + 528, s.width);
  Put32(base, owner + 532, s.height);
  Put32(base, screen, s.width);
  Put32(base, screen + 4, s.height);
  const uint32_t manager = Get32(base, screen + 32);
  if (manager) {
    for (uint32_t i = 0; i < kPoolEntries; ++i) {
      const uint32_t entry = Get32(base, manager + 4 * i);
      if (entry) {
        {
          std::lock_guard lock(queued_mutex);
          queued_entries.insert(entry);
        }
        Call(ctx, base, __imp__sub_820F2A08, frame, {kDeferredQueue, entry});
      }
      Put32(base, manager + 4 * i, 0);
    }
    Call(ctx, base, __imp__sub_8210BA38, frame, {manager});
  }
  Call(ctx, base, __imp__sub_820EE9C8, frame, {kRenderState, 0, 0, s.width, s.height});
  if (!PatchScreenData(base, s))
    REXLOG_WARN("[narutorise] widescreen resize: the screen data (effect aspect, thumbnail entry) NOT patched");
  WriteCamera(base, s);
  SetLayout(s);
  REXLOG_INFO("[narutorise] widescreen resize {}x{} -> {}x{}: {} (front {:08X}, color {:08X}, depth {:08X}, half depth "
              "{:08X}, pool {:08X})", previous.width, previous.height, s.width, s.height, created ? "applied" : "refused",
              t.front, t.color, t.depth, t.half_depth, manager);
  return created ? Outcome::kApplied : Outcome::kRefused;
}

// True once for an entry step B queued; `last` when it was the last one.
bool TakeQueuedEntry(uint32_t entry, bool& last) {
  std::lock_guard lock(queued_mutex);
  if (!queued_entries.erase(entry)) return false;
  last = queued_entries.empty();
  return true;
}
}  // namespace

// Step A: the main thread starts a view block.
REX_HOOK_RAW(sub_8259E908) {
  if (gpu::Active() && SessionStarted() && !pending.load(std::memory_order_acquire) && WindowSettled() &&
      PreviousChangeSettled()) {
    const Size next = Requested();
    if (!(next == Layout()) && PackSize(next) != refused.load(std::memory_order_relaxed)) {
      WriteCamera(base, next);
      SetLayout(next);
      pending.store(PackSize(next), std::memory_order_release);
      REXLOG_INFO("[narutorise] widescreen resize requested: {}x{} -> {}x{}", Current().width, Current().height,
                  next.width, next.height);
    }
  }
  __imp__sub_8259E908(ctx, base);
}

// Step B: the frame delivery's rotation of the queues (820ED9D8, called only
// by 820EFFB8 inside its device lock, the render thread waiting).
REX_HOOK_RAW(sub_820ED9D8) {
  __imp__sub_820ED9D8(ctx, base);
  if (!gpu::Active()) return;
  NoteDelivery();
  const uint32_t count = since_change.load(std::memory_order_relaxed);
  if (count < kSettleLimit) since_change.store(count + 1, std::memory_order_relaxed);
  const uint64_t next = pending.exchange(0, std::memory_order_acq_rel);
  if (!next || UnpackSize(next) == Current()) return;
  switch (Recreate(ctx, base, UnpackSize(next))) {
    case Outcome::kApplied:
      refused.store(0, std::memory_order_relaxed);
      since_change.store(0, std::memory_order_relaxed);
      break;
    case Outcome::kRefused:
      refused.store(next, std::memory_order_relaxed);
      since_change.store(0, std::memory_order_relaxed);
      break;
    case Outcome::kNotReady:
      break;
  }
}

// A header the game binds as one of its targets now (+504 .. +524).
bool CurrentTarget(uint8_t* base, uint32_t header) {
  const uint32_t owner = Get32(base, kDeviceOwner);
  if (!owner) return false;
  for (uint32_t field : {504u, 516u, 520u, 524u})
    if (Get32(base, owner + field) == header) return true;
  return false;
}

// Step C: the deferred destruction of a pool entry (r3 = entry). The game
// releases the base's memory and surface; for an entry step B queued, the
// native renderer also lets go of the embedded texture header (E+32, never
// registered nor through D750: the own texture of its resolves and its
// sampled copy) and, for an alias (E+108 != 0), of the surface the game keeps
// (E+4), unless that header is one of the game's targets by now.
REX_HOOK_RAW(sub_820F2C70) {
  const uint32_t entry = ctx.r3.u32;
  bool last = false;
  if (gpu::Active() && entry && TakeQueuedEntry(entry, last)) {
    std::string evidence;
    const uint32_t header = entry + 32;
    if (!gpu::ReleaseResolveDestination(header, evidence) || !gpu::ReleaseSampledCopy(base, header, evidence))
      REXLOG_WARN("[narutorise] widescreen: pool entry {:08X} texture not released: {}", entry, evidence);
    if (Get32(base, entry + 108)) {
      const uint32_t surface = Get32(base, entry + 4);
      if (surface && !CurrentTarget(base, surface) && !gpu::ReleaseRegistered(surface, evidence))
        REXLOG_WARN("[narutorise] widescreen: alias surface {:08X} not released: {}", surface, evidence);
    }
  }
  __imp__sub_820F2C70(ctx, base);
  if (last) REXLOG_INFO("[narutorise] widescreen: the previous pool is released");
}
