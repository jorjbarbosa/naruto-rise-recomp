// The title's destructions after the drawing of the delivered lists (destruction_order.h).
#include "gpu/d3d/destruction_order.h"
#include "gpu/common/guest_memory.h"
#include "gpu/interpolation/tick.h"
#include <fmt/format.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string_view>

extern "C" void __imp__sub_820EFFB8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8267DDC0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8267DA90(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82659F40(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8266BA98(PPCContext& __restrict, uint8_t*);

namespace gpu::destruction_order {
namespace {
constexpr uint32_t kDrawFlag = 0x82C0F038;          // 1 when the delivery gives the render thread lists to draw.
constexpr uint32_t kDeviceLock = 0x82C0F008;        // The device lock: +4 its owner's thread id, -1 when free.
constexpr uint32_t kDestructionQueue = 0x82C0EBF4;  // The queue of 82659060: +4 its count.
constexpr uint32_t kDisplayDeferred = 0x82C0EBE0;   // The count of 82659F40's objects (queued by 82659FA0).
constexpr uint32_t kSectors = 0x8272F2B0;           // 8267DA90 switches nothing without it and its +44.
constexpr auto kWaitMax = std::chrono::milliseconds(100);
constexpr uint64_t kReportEvery = 3600;             // Sector switches (one per tick) between two reports.

enum Site : unsigned { kDeferred, kDisplay, kSectorSwitch, kSites };
constexpr const char* kSiteNames[kSites] = {"deferred", "display", "sector_switch"};
struct Counters {
  // calls: with work to do; between_ticks: in a frame without a tick; undrawn: lists not yet drawn at
  // the call; exposed: destroys or frees that ran with lists still not drawn (no wait, the lock held, a
  // timeout).
  uint64_t calls = 0, between_ticks = 0, undrawn = 0, waits = 0, wait_us = 0, timeouts = 0, held_lock = 0;
  uint64_t destroys = 0, frees = 0, exposed = 0;
};

const bool enabled = [] {
  const char* v = std::getenv("REX_NATIVE_DESTRUCTION_ORDER");
  return !(v && std::string_view(v) == "0");
}();
std::mutex mutex;
std::condition_variable drawn_cv;
uint64_t delivered = 0, drawn = 0;  // The lists given to the render thread, and those it has drawn.
std::array<Counters, kSites> counters;
// The destruction the thread runs, for the destroys and frees inside it.
struct Scope {
  uint64_t destroys = 0, frees = 0;
};
thread_local Scope* scope = nullptr;

// The thread id the title's lock records (822DCE28: [[r13 + 256] + 332]).
uint32_t TitleThreadId(const PPCContext& ctx, uint8_t* base) {
  const uint32_t thread = Get32(base, ctx.r13.u32 + 256);
  return thread ? Get32(base, thread + 332) : 0;
}

// Before a destruction with work: waits for the drawing of the delivered
// lists. True when the destruction runs with lists still not drawn.
bool WaitForDrawing(PPCContext& ctx, uint8_t* base, Site site) {
  const uint32_t lock = Get32(base, kDeviceLock);
  const bool between_ticks = tick::InGatedStep() && !tick::Current().tick;  // The main thread's frame.
  std::unique_lock guard(mutex);
  Counters& c = counters[site];
  ++c.calls;
  if (between_ticks) ++c.between_ticks;
  if (drawn >= delivered) return false;
  ++c.undrawn;
  if (!enabled) return true;
  if (lock && Get32(base, lock + 4) == TitleThreadId(ctx, base)) {
    ++c.held_lock;
    return true;
  }
  const uint64_t target = delivered;
  const auto start = std::chrono::steady_clock::now();
  const bool done = drawn_cv.wait_for(guard, kWaitMax, [target] { return drawn >= target; });
  if (!done && ++c.timeouts <= 10)
    REXLOG_WARN("[narutorise] {}: the lists were not drawn within {} ms (timeout {})", kSiteNames[site],
                kWaitMax.count(), c.timeouts);
  ++c.waits;
  c.wait_us += uint64_t(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
  return !done;
}

// A destruction of the title: the wait when it has work, its destroys and frees counted.
template <class Original>
void Destruction(PPCContext& ctx, uint8_t* base, Site site, bool work, Original original) {
  if (!work) {
    original();
    return;
  }
  const bool exposed = WaitForDrawing(ctx, base, site);
  Scope inner;
  Scope* outer = scope;
  scope = &inner;
  original();
  scope = outer;
  bool report = false;
  {
    std::lock_guard guard(mutex);
    Counters& c = counters[site];
    c.destroys += inner.destroys;
    c.frees += inner.frees;
    if (exposed) c.exposed += inner.destroys + inner.frees;
    report = site == kSectorSwitch && c.calls % kReportEvery == 0;
  }
  if (report) REXLOG_INFO("[narutorise] Destruction order: {{{}}}", Summary());
}
}  // namespace

void ListsDrawn() {
  {
    std::lock_guard guard(mutex);
    ++drawn;
  }
  drawn_cv.notify_all();
}

void NoteFree() {
  if (scope) ++scope->frees;
}

std::string Summary() {
  std::lock_guard guard(mutex);
  std::string out = fmt::format("\"waits\":{},\"delivered\":{},\"drawn\":{}", enabled, delivered, drawn);
  for (unsigned i = 0; i < kSites; ++i) {
    const Counters& c = counters[i];
    out += fmt::format(",\"{}\":{{\"calls\":{},\"between_ticks\":{},\"undrawn\":{},\"waits\":{},\"wait_us\":{},"
                       "\"timeouts\":{},\"held_lock\":{},\"destroys\":{},\"frees\":{},\"exposed\":{}}}",
                       kSiteNames[i], c.calls, c.between_ticks, c.undrawn, c.waits, c.wait_us, c.timeouts,
                       c.held_lock, c.destroys, c.frees, c.exposed);
  }
  return out;
}
}  // namespace gpu::destruction_order

REX_HOOK_RAW(sub_820EFFB8) {
  __imp__sub_820EFFB8(ctx, base);
  using namespace gpu::destruction_order;
  if (gpu::Get32(base, kDrawFlag) != 1) return;
  std::lock_guard guard(mutex);
  ++delivered;
}
REX_HOOK_RAW(sub_8267DDC0) {
  using namespace gpu::destruction_order;
  const uint32_t queue = gpu::Get32(base, kDestructionQueue);
  Destruction(ctx, base, kDeferred, queue && gpu::Get32(base, queue + 4),
              [&] { __imp__sub_8267DDC0(ctx, base); });
}
REX_HOOK_RAW(sub_82659F40) {
  using namespace gpu::destruction_order;
  Destruction(ctx, base, kDisplay, gpu::Get32(base, kDisplayDeferred) != 0,
              [&] { __imp__sub_82659F40(ctx, base); });
}
REX_HOOK_RAW(sub_8267DA90) {
  using namespace gpu::destruction_order;
  const uint32_t sectors = gpu::Get32(base, kSectors);
  Destruction(ctx, base, kSectorSwitch, sectors && gpu::Get32(base, sectors + 44),
              [&] { __imp__sub_8267DA90(ctx, base); });
}
REX_HOOK_RAW(sub_8266BA98) {
  if (gpu::destruction_order::scope) ++gpu::destruction_order::scope->destroys;
  __imp__sub_8266BA98(ctx, base);
}
