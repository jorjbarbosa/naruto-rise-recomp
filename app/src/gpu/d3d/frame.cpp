// Frame layer of the D3D boundary: D3DDevice_Swap.
//
// 82138A20 is D3DDevice_Swap(device, pFrontBuffer, pParameters). The original
// cannot run over the discard buffer: its tail F608 closes the ring segment and
// submits it through F530, 8212EBB0 switches the ring segment and waits for the
// previous frame fence (+14924), 821445D8 retrains EDRAM, and VdSwap writes the
// flip into the ring. The own Swap ports the CPU effects of the original in its
// order, for the path the title takes (no parameters, no writeback slots
// (+21608), no vblank wait (+22280 bit 2)):
//   - +16544 += 1 and the front-buffer header copy (52 bytes at +14828);
//   - 82146AE0 (debug path) runs original;
//   - presentation is own: the front buffer is read back as frame evidence
//     instead of VdGetSystemCommandBuffer/VdSwap, whose outputs stay zero as on
//     the original path with +21516 set (+21648 = 0, +22276 unchanged);
//   - pending flags +16692: the display mode (bit 3) keeps VdSetDisplayMode,
//     VdGetCurrentDisplayInformation and VdPersistDisplay from the SDK;
//   - 82138848 / 82138698 (display and gamma packets) run over the discard
//     buffer, as do 821441D8 and 821280B0(device, 0) at the end;
//   - the frame batch closes as 8212F2C0 does, on the own queue (CloseFrameBatch),
//     and +14924 holds the frame fence;
//   - the frame is presented at once (the Present is posted right after the
//     frame lot) and the Swap paces itself with a limiter on the title's
//     presentation interval (+13580 vblanks of 60 Hz; 2 in the START, 30
//     Swaps/s). The sync interrupt that 8212F7E0 captured for 821384F0
//     (82138358) is not delivered: no vblank or flip is emulated.
//     The first Swap starts the own frame completion thread, which publishes
//     each frame fence when its lot completed on the own queue.

#include "gpu/d3d/frame.h"
#include "diagnostics/incident.h"
#include "gpu/diagnostics/diagnostics.h"
#include "gpu/d3d/hooks.h"
#include "gpu/d3d/device.h"
#include "gpu/d3d/resources.h"
#include "gpu/d3d/draw.h"
#include "gpu/textures/textures.h"
#include "gpu/buffers/buffers.h"
#include "gpu/diagnostics/census.h"
#include "gpu/diagnostics/frame_trace.h"
#include "gpu/diagnostics/start_evidence.h"
#include "gpu/diagnostics/frame_capture.h"
#include "gpu/interpolation/tick.h"
#include "gpu/diagnostics/measure.h"
#include "gpu/diagnostics/frame_state.h"
#include "gpu/diagnostics/sampler.h"
#include "gpu/d3d/present.h"
#include "gpu/settings.h"
#include "gpu/common/platform.h"
#include "gpu/common/sha256.h"
#include <rex/ui/window.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>
#include <rex/system/xmemory.h>
#include <functional>
#include <thread>
#include <rex/runtime.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

using namespace gpu;

namespace gpu {
std::atomic<uint64_t> swaps{0};  // Own Swaps of the frame layer.
uint32_t Rotl(uint32_t v, unsigned n) { return n ? (v << n) | (v >> (32 - n)) : v; }

// Runs an original over the discard buffer with up to eight argument
// registers; the caller's register file is restored afterwards.
uint32_t CallOverDiscard(PPCContext& ctx, uint8_t* base, uint32_t dev, GuestFunction fn, uint32_t sp,
                         std::initializer_list<uint32_t> args, std::string_view name) {
  std::array<uint8_t, sizeof(PPCContext)> saved{};
  std::memcpy(saved.data(), &ctx, sizeof(PPCContext));
  ctx.r1.u64 = sp;
  decltype(&ctx.r3) regs[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
  unsigned i = 0;
  for (uint32_t a : args)
    if (i < 8) regs[i++]->u64 = a;
  const uint32_t discarded = RunOverDiscard(ctx, base, dev, fn, name);
  std::memcpy(&ctx, saved.data(), sizeof(PPCContext));
  return discarded;
}

// Calls a kernel import with two arguments and returns r3.
uint32_t CallImport(PPCContext& ctx, uint8_t* base, GuestFunction fn, uint32_t sp, uint32_t r3, uint32_t r4) {
  std::array<uint8_t, sizeof(PPCContext)> saved{};
  std::memcpy(saved.data(), &ctx, sizeof(PPCContext));
  ctx.r1.u64 = sp;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  fn(ctx, base);
  const uint32_t result = ctx.r3.u32;
  std::memcpy(&ctx, saved.data(), sizeof(PPCContext));
  return result;
}


uint64_t dropped_sync_interrupts = 0;  // Render thread only.
// The limiter of the own Swap. The period is the title's presentation interval
// (+13580, in vblanks of 60 Hz); a Swap that arrives after its deadline starts
// a new phase instead of catching up in a burst. The wait is the finest timer
// of the system (platform::Sleep) up to about 1 ms before the deadline, then a
// spin on the tick clock. Render thread only.
struct SwapLimiter {
  int64_t frequency = 0, next = 0;
  uint32_t interval = 0, rate = 0;  // rate: the frame rate of the state interpolation mode, else 0.
  uint64_t paced = 0, waited = 0, late = 0, max_late_us = 0, timer_failures = 0;
} swap_limiter;
std::string LimiterSummary() {
  const auto& l = swap_limiter;
  return fmt::format("{{\"interval\":{},\"rate\":{},\"paced\":{},\"waited\":{},\"late\":{},\"max_late_us\":{},"
                     "\"timer_failures\":{},\"period\":\"{}\"}}", l.interval, l.rate, l.paced, l.waited, l.late,
                     l.max_late_us, l.timer_failures, l.rate ? "1/rate s" : "interval/60 s");
}
// Frame-time meter in the window title: fps, mean
// and max frame time of the last half second, measured between the ends of
// consecutive own Swaps (after the limiter, the ticks of SwapLimiter), written
// twice per second as "<title> | 30.0 fps | 33.3 ms (max 36.1)". Fire and
// forget on the UI thread (CallInUIThreadDeferred, values captured by value);
// the title is read there once. Off by default, so the title keeps only the
// game's name and version; REX_NATIVE_FPS_TITLE=1 turns it on. Render thread
// only; the image is untouched.
struct FrameMeter {
  int64_t last = 0, window_start = 0, sum = 0, max = 0;
  uint32_t frames = 0;
  int enabled = -1;
} frame_meter;
void MeterSwap() {
  auto& m = frame_meter;
  if (m.enabled < 0) {
    const char* text = std::getenv("REX_NATIVE_FPS_TITLE");
    m.enabled = text && std::string_view(text) == "1" ? 1 : 0;
  }
  const int64_t f = swap_limiter.frequency;
  if (!m.enabled || !f) return;
  const int64_t now = platform::Ticks();
  if (m.last) {
    const int64_t d = now - m.last;
    m.sum += d;
    m.max = std::max(m.max, d);
    ++m.frames;
  }
  m.last = now;
  if (!m.window_start) m.window_start = now;
  if (now - m.window_start < f / 2 || !m.frames || !m.sum) return;
  const double mean_ms = 1000.0 * double(m.sum) / double(m.frames) / double(f);
  const double max_ms = 1000.0 * double(m.max) / double(f);
  const double fps = double(m.frames) * double(f) / double(m.sum);
  m.sum = m.max = 0;
  m.frames = 0;
  m.window_start = now;
  auto* runtime = rex::Runtime::instance();
  auto* app = runtime ? runtime->app_context() : nullptr;
  if (!DisplayWindow() || !app) return;
  const std::string mode = gpu::tick::Label();  // REX_NATIVE_STATE_INTERPOLATION.
  app->CallInUIThreadDeferred([fps, mean_ms, max_ms, mode] {
    auto* window = DisplayWindow();
    if (!window) return;
    static std::string base_title;  // UI thread only.
    static bool title_read = false;
    if (!title_read) {
      base_title = window->GetTitle();
      title_read = true;
    }
    std::string text = fmt::format("{} | {:.1f} fps | {:.1f} ms (max {:.1f})", base_title, fps, mean_ms, max_ms);
    if (!mode.empty()) text += " | " + mode;
    window->SetTitle(text);
  });
}
// The interval the own Swap is paced at when it is not the title's: 1 with the
// state interpolation mode (tick.h, when its gate is switched off by F8), or
// the diagnostic REX_NATIVE_SWAP_INTERVAL (1..4), which wins when set. 0 keeps
// the title's +13580. The 1/60 s simulation of the open world is not a product
// option: it holds Naruto at the wall of the first mission's bridge.
uint32_t ForcedSwapInterval() {
  static const uint32_t environment = [] {
    const char* text = std::getenv("REX_NATIVE_SWAP_INTERVAL");
    const int n = text ? std::atoi(text) : 0;
    return n >= 1 && n <= 4 ? uint32_t(n) : 0u;
  }();
  if (environment) return environment;
  return tick::Enabled() ? 1u : 0u;
}
// The engine's time update (826ACFC0, main thread,
// r3 = the time structure whose pointer is at 82C0F080) measures the dt from
// the QPC tick unless the fixed-step flag (+72) is set, when the dt is +76.
// The open world runs with the fixed step of the title's interval (2/60 s),
// so a Swap paced at 1/60 s would double the game speed.
// With a forced interval (ForcedSwapInterval), +76 is
// rewritten from the title's interval / 60 s to the forced interval / 60 s
// before each update, only when it holds the title's value (a dt set by a
// script stays). The fields are recorded as native-d3d-time-step: the first
// calls, every 300th, and every change of the mode.
// REX_NATIVE_FIXED_STEP_REWRITE=0 only records them.
// The adaptive step. With the fixed step at 1/60 s every frame over 16.7 ms
// would delay the game (slow motion). The game time follows the real time:
// the real time since the previous update is owed, and the step is the
// multiple of the
// forced interval nearest to the debt, from one forced interval to the
// title's (1/60 or 2/60 s in the open world: only the title's own steps).
// The debt is capped at that plus half a frame, so a long hitch (a load) is
// not fast-forwarded. Only when the
// forced interval divides the title's (the open world at 60, not the fight,
// which already runs 1/60). REX_NATIVE_ADAPTIVE_STEP=0 writes the forced
// interval at every update.
struct TimeStepProbe {
  uint64_t calls = 0, rewrites = 0, fixed_calls = 0, foreign_fixed = 0;
  uint32_t last_fixed = 0xFFFFFFFF, last_interval = 0;
  bool rewrite = true, adaptive = true;
  int enabled = -1;
  // Adaptive step: the structure and the value of the last write, the time of
  // the previous update and the real time owed to the game, in seconds.
  uint32_t owned_time = 0;
  float written = 0;
  uint64_t previous_ns = 0;
  double debt = 0;
  uint64_t adaptive_calls = 0, multiple_steps = 0;
  double max_debt = 0;
  // Over the own fixed step: the real time between updates and the game time
  // given (simulated / real below 1 is slow motion).
  double real_s = 0, simulated_s = 0;
} time_step_probe;  // Main thread only.
void TimeStepHook(PPCContext& ctx, uint8_t* base) {
  sampler::RegisterThread("main");  // REX_NATIVE_SAMPLE_PERIOD_US.
  auto& p = time_step_probe;
  if (p.enabled < 0) {
    const char* text = std::getenv("REX_NATIVE_FIXED_STEP_REWRITE");
    p.rewrite = !(text && std::string_view(text) == "0");
    const char* adaptive = std::getenv("REX_NATIVE_ADAPTIVE_STEP");
    p.adaptive = !(adaptive && std::string_view(adaptive) == "0");
    p.enabled = 1;
  }
  const uint32_t time = ctx.r3.u32;
  if (!time) return;
  const uint32_t context = Get32(base, 0x82C0F008);
  const uint32_t title_interval = context ? Get32(base, context + 448) : 0;
  // The state interpolation mode (tick.h) owns the time: the step stays the
  // title's, and a value written while its gate was off (F8) goes back to it.
  const bool interpolation = tick::Owning();
  const uint32_t forced = interpolation ? 0u : ForcedSwapInterval();
  const uint32_t fixed = Get32(base, time + 72);
  float dt_fixed = GetFloat(base, time + 76);
  ++p.calls;
  bool rewritten = false;
  uint32_t step_units = 0;
  const uint64_t now_ns = profile::Now();
  bool owned = false;
  if (fixed) {
    ++p.fixed_calls;
    if (interpolation && p.owned_time == time && p.written > 0 && title_interval >= 1 && title_interval <= 4 &&
        std::isfinite(dt_fixed) && std::fabs(dt_fixed - p.written) <= p.written * 0.01f) {
      dt_fixed = float(title_interval) / 60.0f;
      PutFloat(base, time + 76, dt_fixed);
      ++p.rewrites;
      rewritten = true;
    }
    if (forced && p.rewrite && title_interval >= 1 && title_interval <= 4 && forced != title_interval) {
      const float title_dt = float(title_interval) / 60.0f, forced_dt = float(forced) / 60.0f;
      auto matches = [](float a, float b) { return std::fabs(a - b) <= b * 0.01f; };
      // The title's value (written on entering the world and after the fight)
      // or the last own write; anything else is a script's dt and stays.
      const bool own_value = p.owned_time == time && p.written > 0 && matches(dt_fixed, p.written);
      if (std::isfinite(dt_fixed) && (matches(dt_fixed, title_dt) || own_value)) {
        float dt = forced_dt;
        const uint32_t max_units = title_interval % forced == 0 ? title_interval / forced : 1;
        const bool continuing = p.owned_time == time && p.previous_ns;
        const double elapsed = continuing ? double(now_ns - p.previous_ns) * 1e-9 : 0.0;
        if (continuing) {
          p.real_s += elapsed;
          p.simulated_s += p.written;  // The step given at the previous update.
        }
        if (p.adaptive && max_units > 1) {
          const double unit = double(forced) / 60.0;
          // A new run of the fixed step starts in phase.
          p.debt = continuing ? std::min(p.debt + elapsed, (max_units + 0.5) * unit) : unit;
          p.max_debt = std::max(p.max_debt, p.debt);
          step_units = uint32_t(std::clamp<long>(std::lround(p.debt / unit), 1, long(max_units)));
          p.debt = std::max(p.debt - step_units * unit, -unit);
          dt = float(step_units) * forced_dt;
          ++p.adaptive_calls;
          if (step_units > 1) ++p.multiple_steps;
        }
        PutFloat(base, time + 76, dt);
        dt_fixed = dt;
        p.owned_time = time;
        p.written = dt;
        owned = true;
        ++p.rewrites;
        rewritten = true;
      } else if (std::isfinite(dt_fixed) && !matches(dt_fixed, forced_dt)) {
        ++p.foreign_fixed;
      }
    }
  }
  if (owned) {
    p.previous_ns = now_ns;
  } else {
    p.owned_time = 0;  // Out of the own fixed step: the next run starts again.
    p.previous_ns = 0;
  }
  const bool changed = fixed != p.last_fixed || title_interval != p.last_interval;
  p.last_fixed = fixed;
  p.last_interval = title_interval;
  if (Diagnostics() && (p.calls <= 8 || p.calls % 300 == 0 || changed)) {
    auto finite = [](float f) { return std::isfinite(f) ? double(f) : 0.0; };
    gpu::host::Record("native-d3d-time-step", fmt::format(
        "\"calls\":{},\"fixed\":{},\"dt_fixed\":{:.7f},\"dt\":{:.7f},\"dt_raw\":{:.7f},\"dt_max\":{:.7f},"
        "\"scale\":{:.4f},\"title_interval\":{},\"forced_interval\":{},\"rewritten\":{},\"rewrites\":{},"
        "\"fixed_calls\":{},\"foreign_fixed\":{},\"changed\":{},\"adaptive\":{},\"step_units\":{},"
        "\"adaptive_calls\":{},\"multiple_steps\":{},\"debt_ms\":{:.3f},\"max_debt_ms\":{:.3f},"
        "\"real_s\":{:.3f},\"simulated_s\":{:.3f},\"interpolation\":{}",
        p.calls, fixed, finite(dt_fixed), finite(GetFloat(base, time + 64)), finite(GetFloat(base, time + 68)),
        finite(GetFloat(base, time + 80)), finite(GetFloat(base, time + 88)), title_interval, forced, rewritten,
        p.rewrites, p.fixed_calls, p.foreign_fixed, changed, p.adaptive, step_units, p.adaptive_calls,
        p.multiple_steps, p.debt * 1e3, p.max_debt * 1e3, p.real_s, p.simulated_s, interpolation));
  }
}
void PaceSwap(PPCContext& ctx, uint8_t* base, uint32_t dev) {
  profile::Timer pace_time(profile::kSwapPace);
  auto& l = swap_limiter;
  // A forced interval (ForcedSwapInterval: REX_NATIVE_SWAP_INTERVAL=<1..4>)
  // paces the own Swap at that interval instead of the title's +13580, which
  // stays untouched (the title keeps requesting 2 in the menus and the open
  // world, 1 in the fight). The engine's fixed step follows it (TimeStepHook).
  // The state interpolation mode (tick.h) paces at its frame rate instead, or
  // at the tick rate when its gate is off under the fixed step (PaceRate).
  const uint32_t forced_interval = ForcedSwapInterval();
  const uint32_t rate = tick::Owning() ? tick::PaceRate() : 0u;
  uint32_t interval = forced_interval ? forced_interval : Get32(base, dev + 13580);
  if (!rate && (interval < 1 || interval > 4)) {  // Clamped to the covered range.
    UncoveredOperation(ctx, fmt::format("Swap-presentation-interval-uncovered-{}", interval));
    interval = std::clamp(interval, 1u, 4u);
  }
  if (!l.frequency) l.frequency = platform::TickFrequency();
  if (interval != l.interval || rate != l.rate) l.next = 0;  // A new period starts a new phase.
  l.interval = interval;
  l.rate = rate;
  const int64_t period = rate ? l.frequency / rate : int64_t(interval) * l.frequency / 60;
  int64_t now = platform::Ticks();
  ++l.paced;
  if (l.next && now < l.next) {
    const int64_t remaining = l.next - now, margin = l.frequency / 1000;
    if (remaining > margin && !platform::Sleep(remaining - margin)) ++l.timer_failures;
    do {
      platform::Pause();
      now = platform::Ticks();
    } while (now < l.next);
    ++l.waited;
    l.next += period;
  } else {
    if (l.next) {
      ++l.late;
      l.max_late_us = std::max<uint64_t>(l.max_late_us, uint64_t((now - l.next) * 1000000 / l.frequency));
    }
    l.next = now + period;
  }
}
}  // namespace gpu

extern "C" void __imp__sub_82138A20(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82146AE0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82138848(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82138698(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_821441D8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_821280B0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__VdSetDisplayMode(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__VdGetCurrentDisplayInformation(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__VdPersistDisplay(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__MmFreePhysicalMemory(PPCContext& __restrict, uint8_t*);
// 826ACFC0: the engine's time update (TimeStepHook above).
extern "C" void __imp__sub_826ACFC0(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_826ACFC0) {
  if (gpu::Active()) gpu::TimeStepHook(ctx, base);
  __imp__sub_826ACFC0(ctx, base);
}
REX_HOOK_RAW(sub_82138A20) {
  narutorise::incident::Activity activity(narutorise::incident::Role::kGuest);
  if (!gpu::Active()) {
    PacingAtSwap(ctx, base, ctx.r3.u32, false);  // Pacing samples of the Xenos mode.
    // Frame state; a front buffer to capture is read after the original.
    const uint32_t state_dev = ctx.r3.u32, state_front = ctx.r4.u32;
    const int64_t capture = StateAtSwap(ctx, base, state_dev, false, state_front);
    std::array<uint32_t, 6> front_fetch{};
    if (capture >= 0)
      for (unsigned i = 0; i < 6; ++i) front_fetch[i] = Get32(base, state_front + 28 + 4 * i);
    __imp__sub_82138A20(ctx, base);
    if (capture >= 0) StateAfterSwapReference(ctx, base, state_dev, front_fetch, capture);
    return;
  }
  HookScope scope("Swap");
  if (!device_owner || !device_owner->live() || ctx.r3.u32 != device_owner->device()) StopInOrder(ctx, "Swap-device", 88);
  TakeStopRequest(ctx);  // The watchdog stop, in order on this thread.
  TakePendingDraw(&ctx, base, "Swap");  // A pending BeginVertices quad goes into this frame.
  gpu::sampler::RegisterThread("swap");  // The frames of the samples.
  gpu::sampler::NoteSwap(own_draws.load());
  PacingAtSwap(ctx, base, ctx.r3.u32, true);  // The frame that just ended.
  StateAtSwap(ctx, base, ctx.r3.u32, true, ctx.r4.u32);  // Its state and front buffer.
  const bool measuring = MeasureSwaps() > 0;
  if (measuring) MeasureAtSwap(ctx);  // The frame closed by this Swap.
  gpu::profile::Timer swap_time(gpu::profile::kSwap);
  const uint32_t dev = ctx.r3.u32, front = ctx.r4.u32, params = ctx.r5.u32, lr = ctx.lr;
  if (!front) StopInOrder(ctx, "Swap-front-buffer-null", 88);
  // Not covered: without the census these are not read (the own limiter
  // paces the frames).
  if (params) UncoveredOperation(ctx, "Swap-parameters-uncovered");
  if (Get32(base, dev + 21608)) UncoveredOperation(ctx, "Swap-writeback-slots-uncovered");
  if (Get32(base, 0x82C0F1A0)) UncoveredOperation(ctx, "Swap-global-flush-uncovered");
  if (Get32(base, dev + 22280) & 4) UncoveredOperation(ctx, "Swap-vblank-wait-uncovered");
  // The START lot stops the render thread at its fifth draw. Seven own
  // Swaps after the frame where the chosen occurrence was due (one lot per
  // frame) without it, the census stops here in order with that reason.
  if (!measuring && start_draws.load() && swaps.load() >= start_first_swap.load() + StartStopOccurrence() + 7)
    StopInOrder(ctx, "native-d3d-start-occurrence-unmatched", 86);
  const uint32_t frame = (ctx.r1.u32 - 1024) & ~15u;  // Below the caller's frame.
  struct SwapScope {
    SwapScope() { gpu::own_swap_scope = true; }
    ~SwapScope() { gpu::own_swap_scope = false; }
  } swap_scope;
  Put32(base, dev + 16544, Get32(base, dev + 16544) + 1);
  // Front-buffer fetch constant (header +28), adjusted as the original does for VdSwap.
  std::array<uint32_t, 6> f{};
  for (unsigned i = 0; i < 6; ++i) f[i] = Get32(base, front + 28 + 4 * i);
  uint32_t f1 = f[1];
  if ((f1 & 0x3F) == 50) f1 = (f1 & ~0x3Fu) | 6;
  else if ((f1 & 0x3F) == 7) f1 = (f1 & ~0x3Fu) | 54;
  const uint32_t mode13592 = Get32(base, dev + 13592);
  const uint32_t low = Rotl(mode13592, 15) & 0x7FFF;
  const uint32_t merged = (low & ~0xFFFFE001u) | (f[3] & 0xFFFFE001u);
  const uint32_t f3 = (low & 1) | (merged & 0xFFFFFFFEu);
  std::memcpy(base + dev + 14828, base + front, 52);
  const uint32_t extra = (Rotl(f3, 2) & 2) + 1;
  const uint32_t width = (f[2] & 0x1FFF) + extra, height = ((f[2] >> 13) & 0x1FFF) + extra;
  const uint32_t front_base = f1 & 0xFFFFF000u;
  CallGuest(ctx, base, __imp__sub_82146AE0, frame, dev, front);
  const uint64_t n = swaps.fetch_add(1) + 1;
  narutorise::incident::Progress(narutorise::incident::Role::kGuest);
  if (n == 1) AttachPresentation(ctx);  // Own swapchain on the SDK window.
  MaybePostClose(n);  // REX_NATIVE_CLOSE_AFTER_SWAPS, the window close without a person.
  // START evidence: after the first own draw of the START lot, three more
  // readbacks, 30 swaps apart.
  static uint64_t start_reads = 0, next_start_read = 0;
  const bool start_read = !measuring && start_draws.load() && start_reads < 3 && n >= next_start_read;
  if (start_read) {
    ++start_reads;
    next_start_read = n + 30;
  }
  // No front-buffer read in the measurement mode, except the partial
  // images of the guard census.
  const bool census_read = CensusFrontRead(n);
  const std::string front_record = (Diagnostics() && !measuring && (n <= kFrontReadbacks || start_read)) || census_read
                                       ? ReadFrontBuffer(front, n) : "null";
  {  // Frame trace: the frame this Swap closes.
    static uint64_t draws_at_last_swap = 0;
    const uint64_t draws_now = own_draws.load();
    FrameTraceNoteSwap(n, draws_now - draws_at_last_swap);
    TransitionCaptureAtSwap(front, n, draws_now - draws_at_last_swap);
    draws_at_last_swap = draws_now;
    {  // Idle buffer entries leave, retired buffers are released after the frame's draws, on the stage.
      auto* dispatcher = gpu::host::ActiveDispatcher();
      if (dispatcher) dispatcher->Defer([n] { BufferRegistrySweep(n); });
      else BufferRegistrySweep(n);
    }
    if (FrameTraceTarget() == n) {
      FrameTraceReadback(front, n);
      FrameTraceRearm();  // The key mode traces again at the next F10.
    }
  }
  Put32(base, dev + 21648, 0);
  uint32_t flags = Get32(base, dev + 16692);
  const uint32_t pending = flags;
  // The gamma ramp of the title (D3DGAMMARAMP, 3 x 256 big-endian words at
  // +14988), recorded when SetGammaRamp marked it pending (bit 1), first 4 times.
  static uint32_t gamma_records = 0;
  if (Diagnostics() && (pending & 2) && gamma_records < 4) {
    ++gamma_records;
    std::string words;
    for (uint32_t i = 0; i < 768; ++i) words += fmt::format("{}{}", i ? "," : "", Get16(base, dev + 14988 + 2 * i));
    gpu::host::Record("native-d3d-gamma-ramp", fmt::format(
        "\"swap\":{},\"pending16692\":{},\"ramp_rgb_256_words\":[{}]", swaps.load() + 1, pending, words));
  }
  // The pending ramp goes to the final pass of the presentation.
  if (pending & 2) PostGammaRamp(ctx, base, dev);
  Put32(base, dev + 16692, 0);
  std::string display_record = "null";
  if (flags & 8) {
    const uint32_t format = mode13592 & 0x3F;
    uint32_t mode = 0x40000000 | (Rotl(Get32(base, dev + 13596), 28) & 0x30000000) |
                    (format == 7 || format == 54 ? 0x08000000 : 0);
    if (Get32(base, dev + 13528) == 720 && (Get32(base, dev + 13532) == 480 || Get32(base, dev + 13532) == 576))
      mode |= 2;
    const uint32_t info = frame + 368;
    const uint32_t set = CallImport(ctx, base, __imp__VdSetDisplayMode, frame, mode, 0);
    CallImport(ctx, base, __imp__VdGetCurrentDisplayInformation, frame, info, 0);
    Put32(base, dev + 21524, Get16(base, info + 72));
    Put32(base, dev + 21528, Get16(base, info + 74));
    Put32(base, dev + 21532, Get16(base, info + 86));
    display_record = fmt::format("{{\"mode\":{},\"set_result\":{},\"info\":[{},{},{}]}}", mode, set,
                                 Get16(base, info + 72), Get16(base, info + 74), Get16(base, info + 86));
  }
  const uint32_t front_physical = GuestPhysicalAlias(front_base);
  if (Get8(base, dev + 10942) & 0x10) {
    std::memset(base + dev + 13652, 0, 56);
    flags |= 4;
    Put8(base, dev + 10942, Get8(base, dev + 10942) & 0xEF);
  }
  const uint8_t b43 = Get8(base, dev + 10943);
  if (b43 & 0x40) flags |= 3;
  Put8(base, dev + 10943, b43 & 0xBF);
  uint32_t discarded = 0;
  if (flags)
    discarded += CallOverDiscard(ctx, base, dev, __imp__sub_82138848, frame,
                                 {dev, flags, front_physical, mode13592, width, height, dev + 13708, dev + 14988},
                                 "Swap-82138848");
  else
    discarded += CallOverDiscard(ctx, base, dev, __imp__sub_82138698, frame, {dev, mode13592}, "Swap-82138698");
  std::string persist_record = "null";
  if (flags & 8) {
    std::memset(base + frame + 184, 0, 12);
    Put32(base, frame + 160, 0);
    const uint32_t persisted = CallImport(ctx, base, __imp__VdPersistDisplay, frame, frame + 184, frame + 160);
    const uint32_t memory = Get32(base, frame + 160);
    if (persisted) CallImport(ctx, base, __imp__MmFreePhysicalMemory, frame, 1, memory);
    persist_record = fmt::format("{{\"result\":{},\"memory\":{},\"freed\":{}}}", persisted, memory, persisted != 0);
  }
  // End of frame: own batch close in place of 8212F608 -> 8212F3A8 -> 8212F2C0.
  // EDRAM retraining (821445D8) and the ring segment switch (8212EBB0, whose
  // own wait on +14924 is skipped because +22280 has bit 0) have no own
  // equivalent.
  // The frame lot closes without waiting; its frame end goes to the own
  // frame completion thread, which publishes the frame fence once the lot
  // completed. The lot close is posted, so a failed posted operation drops it
  // and nothing is published.
  // The front buffer is copied in the frame lot. Its Present
  // is posted right after the lot, on the same owner and queue, so it reads
  // the copy; the next copy into the same stage slot comes two Swaps later.
  const auto present = PostStageFront(ctx, front, n, front_record != "null");
  uint32_t frame_fence = 0;
  std::string fence_fields;
  FrameEvent lot_event = CreateFrameEvent();
  if (!lot_event) StopInOrder(ctx, "Swap-frame-lot-event", 88);
  auto close_lot = [lot_event](uint32_t) {
    auto* dispatcher = gpu::host::ActiveDispatcher();
    return dispatcher && dispatcher->Post([lot_event](Engine& engine, std::string& out) {
      uint64_t target = 0;
      return engine.CloseFrame(lot_event, target, out);
    });
  };
  if (!device_owner->CloseFrameBatch(close_lot, frame_fence, fence_fields)) {
    RecordPostedFailure("Swap-CloseFrameBatch");
    StopInOrder(ctx, "Swap-frame-batch-unproved", 88);
  }
  // The sync interrupt of 821384F0 (82138358, the only routine the
  // leaf admits) is dropped: the Swap presents and paces itself.
  const auto batch_interrupts = TakeLeafInterrupts();
  dropped_sync_interrupts += batch_interrupts.size();
  if (n == 1) {
    std::string start_fields;
    if (!gpu::host::StartInterrupts(dev, device_owner->scratch(), start_fields))
      StopInOrder(ctx, "Swap-own-frame-completion-start", 88);
    gpu::host::Record("native-d3d-interrupts-start", start_fields + fmt::format(
        ",\"swap\":1,\"frame_fence\":{},\"dropped_sync_interrupts\":{}", frame_fence, batch_interrupts.size()));
  }
  std::string interrupt_args;
  for (const auto& interrupt : batch_interrupts)
    interrupt_args += fmt::format("{}[{},{},{},{}]", interrupt_args.empty() ? "" : ",", interrupt.routine,
                                  interrupt.argument, interrupt.cpu_mask, interrupt.lr);
  if (present && !gpu::PresentFrame(present)) {
    RecordPostedFailure("Swap-present");
    StopInOrder(ctx, "Swap-present-post", 88);
  }
  if (!gpu::host::QueueFrameEnd(frame_fence, lot_event, {}, n, nullptr))
    StopInOrder(ctx, "Swap-own-frame-end-queue", 88);
  // As the tail of the original (82139090): BlockOnFence(+14924, 3) on the
  // fence of the previous frame, then +14924 keeps this frame's fence.
  const uint32_t previous_fence = Get32(base, dev + 14924);
  std::string previous_fields = "null";
  if (previous_fence) {
    gpu::profile::Timer wait_time(gpu::profile::kSwapWaitIdle);
    std::string evidence, fields;
    auto drain = [&] {
      auto* dispatcher = gpu::host::ActiveDispatcher();
      return dispatcher && dispatcher->Drain(evidence);
    };
    const auto* dispatcher = gpu::host::ActiveDispatcher();
    const auto preparation_budget = std::chrono::seconds(
        dispatcher && dispatcher->Api() == GraphicsApi::kVulkan ? 60 : 2);
    if (device_owner->BlockOnFence(previous_fence, drain, FenceWaitFailed, fields, preparation_budget) ==
        DeviceOwner::FenceResult::Failed) {
      TakeStopRequest(ctx);  // A watchdog stop asked meanwhile keeps its own reason.
      RecordPostedFailure("Swap-previous-frame-fence");
      StopInOrder(ctx, "Swap-previous-frame-fence-unproved", 88);
    }
    previous_fields = "{" + fields + "}";
    // The previous frame was presented (its Present was posted
    // after its lot) and its lot completed, so it counts as flipped (+16552),
    // as the console counts the flip before this wait returns. The vblank
    // counters (+16532, +16536, +16540, +16548, +21676) stay unchanged: only
    // the interrupt routines, no longer delivered, read them.
    Put32(base, dev + 16552, Get32(base, dev + 16552) + 1);
  }
  Put32(base, dev + 14924, frame_fence);
  Put8(base, dev + 10941, Get8(base, dev + 10941) & 0xEF);
  // 821441D8 calls the callback chains rooted at 8200086C and
  // 820008F8, empty at the device creation (row 17), inside this scope.
  for (const uint32_t root_at : {0x8200086Cu, 0x820008F8u}) {
    const uint32_t root = Get32(base, root_at);
    if (root && Get32(base, root))  // The original below calls them, as on the console.
      UncoveredOperation(ctx, fmt::format("Swap-821441D8-callback-present-{:08X}", root_at));
  }
  discarded += CallOverDiscard(ctx, base, dev, __imp__sub_821441D8, frame, {dev}, "Swap-821441D8");
  discarded += CallOverDiscard(ctx, base, dev, __imp__sub_821280B0, frame, {dev, 0}, "Swap-821280B0");
  static const auto first_swap = std::chrono::steady_clock::now();
  const auto host_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - first_swap).count();
  if (Diagnostics() && (n <= 8 || n % (measuring ? 300 : 30) == 0 || start_read || census_read))
    gpu::host::Record("native-d3d-swap", fmt::format(
        "\"host_ms_since_first_swap\":{},\"own_draws\":{},\"inline_quads\":{},\"kicks\":{},"
        "\"producer_arrivals\":{},\"producer_calls\":{},\"unpredicated_commits\":{},\"texture_locks\":{},"
        "\"texture_uploads\":{},\"texture_binding_reuses\":{},\"texture_anisotropy_forced\":{},\"texture_integral_verify\":{},"
        "\"scene_items\":{},\"scene_flushes\":{},\"start_draws\":{},"
        "\"batch_interrupts\":[{}],\"leaf_interrupts\":{},\"interrupts\":{},\"records\":{},"
        "\"vblank16532\":{},\"swap_interrupts16548\":{},\"flips16552\":{},", host_ms, own_draws.load(),
        InlineDrawCalls(), kicks.load(), producer_arrivals.load(), producer_calls.load(), unpredicated_commits.load(),
        texture_content.Locks(), texture_uploads.load(), texture_binding_reuses.load(),
        texture_anisotropy_forced.load(), TextureVerify(),
        scene_items.load(), scene_flushes.load(), start_draws.load(), interrupt_args,
        leaf_interrupt_count.load(),
        gpu::host::InterruptSummary(), gpu::host::RecordSummary(), Get32(base, dev + 16532), Get32(base, dev + 16548),
        Get32(base, dev + 16552)) + fmt::format(
        "\"swap\":{},\"lr\":{},\"guest_thread\":{},\"front\":{},\"front_physical\":{},\"size\":[{},{}],"
        "\"pending16692\":{},\"flags\":{},\"display\":{},\"persist\":{},{},\"previous_frame_fence\":{},"
        "\"fences\":{},\"discarded_bytes\":{},\"front_buffer\":{},\"vdswap\":false,\"edram_retrain\":false,"
        "\"ring_segment_switch\":false,\"dropped_sync_interrupts\":{},\"limiter\":{}", n, lr, GuestThread(),
        front, front_physical, width, height, pending, flags, display_record, persist_record, fence_fields,
        previous_fields, device_owner->FenceSummary(), discarded, front_record, dropped_sync_interrupts,
        LimiterSummary()));
  if (auto* dispatcher = host::ActiveDispatcher()) ApplyPendingSettings(*dispatcher);
  PaceSwap(ctx, base, dev);  // Last, after the present.
  MeterSwap();  // The frame-time meter of the window title.
}
