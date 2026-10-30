// The tick's deferred destruction after the drawing of its lists (destruction_order.h).
#include "gpu/d3d/destruction_order.h"
#include "gpu/common/guest_memory.h"
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string_view>

extern "C" void __imp__sub_820EFFB8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8267DDC0(PPCContext& __restrict, uint8_t*);

namespace gpu::destruction_order {
namespace {
constexpr uint32_t kDrawFlag = 0x82C0F038;          // 1 when the delivery gives the render thread lists to draw.
constexpr uint32_t kDeviceLock = 0x82C0F008;        // The device lock: +4 its owner's thread id, -1 when free.
constexpr uint32_t kDestructionQueue = 0x82C0EBF4;  // The queue of 82659060: +4 its count.
constexpr auto kWaitMax = std::chrono::milliseconds(100);
constexpr uint64_t kReportEvery = 1800;

const bool enabled = [] {
  const char* v = std::getenv("REX_NATIVE_DESTRUCTION_ORDER");
  return !(v && std::string_view(v) == "0");
}();
std::mutex mutex;
std::condition_variable drawn_cv;
uint64_t delivered = 0, drawn = 0;  // The lists given to the render thread, and those it has drawn.
uint64_t waits = 0, wait_us = 0, timeouts = 0, held_lock = 0;

// The thread id the title's lock records (822DCE28: [[r13 + 256] + 332]).
uint32_t TitleThreadId(const PPCContext& ctx, uint8_t* base) {
  const uint32_t thread = Get32(base, ctx.r13.u32 + 256);
  return thread ? Get32(base, thread + 332) : 0;
}

void WaitForDrawing(PPCContext& ctx, uint8_t* base) {
  const uint32_t queue = Get32(base, kDestructionQueue);
  if (!queue || !Get32(base, queue + 4)) return;
  const uint32_t lock = Get32(base, kDeviceLock);
  std::unique_lock guard(mutex);
  if (drawn >= delivered) return;
  if (lock && Get32(base, lock + 4) == TitleThreadId(ctx, base)) {
    ++held_lock;
    return;
  }
  const uint64_t target = delivered;
  const auto start = std::chrono::steady_clock::now();
  if (!drawn_cv.wait_for(guard, kWaitMax, [target] { return drawn >= target; }) && ++timeouts <= 10)
    REXLOG_WARN("[narutorise] deferred destruction: the lists were not drawn within {} ms (timeout {})",
                kWaitMax.count(), timeouts);
  wait_us += uint64_t(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
  if (++waits % kReportEvery == 0)
    REXLOG_INFO("[narutorise] deferred destruction: {} waits for the drawing ({} us on average), {} timeouts, "
                "{} with the device lock held", waits, wait_us / waits, timeouts, held_lock);
}
}  // namespace

void ListsDrawn() {
  if (!enabled) return;
  {
    std::lock_guard guard(mutex);
    ++drawn;
  }
  drawn_cv.notify_all();
}
}  // namespace gpu::destruction_order

REX_HOOK_RAW(sub_820EFFB8) {
  __imp__sub_820EFFB8(ctx, base);
  using namespace gpu::destruction_order;
  if (!enabled || gpu::Get32(base, kDrawFlag) != 1) return;
  std::lock_guard guard(mutex);
  ++delivered;
}
REX_HOOK_RAW(sub_8267DDC0) {
  if (gpu::destruction_order::enabled) gpu::destruction_order::WaitForDrawing(ctx, base);
  __imp__sub_8267DDC0(ctx, base);
}
