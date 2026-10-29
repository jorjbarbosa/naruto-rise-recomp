// The logic tick of the engine at a free frame rate (tick.h).
#include "gpu/common/platform.h"
#include "gpu/interpolation/tick.h"
#include "gpu/interpolation/transforms.h"
#include "gpu/common/guest_memory.h"
#include "gpu/d3d/d3d.h"
#include "gpu/diagnostics/diagnostics.h"
#include "gpu/diagnostics/motion.h"
#include "gpu/host/services.h"
#include "gpu/settings.h"
#include <fmt/format.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <string_view>

extern "C" void __imp__sub_82684518(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82677D20(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_826843D8(PPCContext& __restrict, uint8_t*);
extern "C" void sub_82684050(PPCContext& __restrict, uint8_t*);
extern "C" void sub_8259E908(PPCContext& __restrict, uint8_t*);
extern "C" void sub_8265A058(PPCContext& __restrict, uint8_t*);
extern "C" void sub_826AC2E8(PPCContext& __restrict, uint8_t*);
extern "C" void sub_8259FF40(PPCContext& __restrict, uint8_t*);

namespace gpu::tick {
namespace {
constexpr uint32_t kTimePointer = 0x82C0F080;
// The view block of 82684518: +8 the world, +12 the view.
constexpr uint32_t kViews = 0x82709C34;
constexpr uint32_t kViewsOff = 0x82C0EBB4;   // Set while a world is replaced: no view.
constexpr uint32_t kWorldEffects = 0x82C0EB04;  // The argument of 826AC2E8.

const bool by_variable = [] { const char* v = std::getenv("REX_NATIVE_STATE_INTERPOLATION"); return v && *v; }();
std::atomic<bool> switched_off{false};
bool f8_down = false;
std::atomic<uint32_t> configured_rate{0};  // Accepted only at the start of a complete logic iteration.
// The limiter's rate (PaceRate), decided on the main thread and read by the
// render thread; 0 before the first decision.
std::atomic<uint32_t> pace_rate{0};
// Main thread.
Frame frame;
bool decided = false, started = false, in_step = false, in_main_step = false;
uint32_t frames_since_tick = 0;
int64_t frequency = 0, last = 0;
double debt = 0;  // Real time owed to the game, seconds.
uint64_t capped = 0;  // Iterations whose debt reached two periods (the game behind the real time).
int64_t window_start = 0;
uint64_t window_ticks = 0, window_iterations = 0;
// The time of the view block (tick.h): 0 off, 1 the image's advance, 2 the
// probe (the tick's dt in the ticks, 0 between them).
int view_time = -1;
struct EngineDt {
  uint64_t us = 0;  // +56, int64.
  float dt = 0.0f, raw = 0.0f;  // +64, +68.
};
EngineDt view_saved, view_set;
double view_ahead = 0;  // ViewAhead(), seconds.
double view_factor = 1;  // ViewFactor().
uint32_t view_address = 0;  // The time structure written, 0 when nothing is.
bool view_started = false;
double view_shown = 0;  // The image's time of the last view block, in periods.
double window_view_dt = 0, window_tick_dt = 0;  // The dt the view blocks saw and the ticks' dt, seconds.

uint64_t Get64(uint8_t* base, uint32_t a) { return (uint64_t(Get32(base, a)) << 32) | Get32(base, a + 4); }
void Put64(uint8_t* base, uint32_t a, uint64_t v) {
  Put32(base, a, uint32_t(v >> 32));
  Put32(base, a + 4, uint32_t(v));
}
EngineDt ReadDt(uint8_t* base, uint32_t time) {
  return {Get64(base, time + 56), GetFloat(base, time + 64), GetFloat(base, time + 68)};
}
void WriteDt(uint8_t* base, uint32_t time, const EngineDt& d) {
  Put64(base, time + 56, d.us);
  PutFloat(base, time + 64, d.dt);
  PutFloat(base, time + 68, d.raw);
}

int ViewTime() {
  if (view_time < 0) {
    const char* text = std::getenv("REX_NATIVE_INTERPOLATION_VIEW_TIME");
    const std::string_view value = text ? text : "";
    view_time = !Enabled() || value == "0" ? 0 : value == "tick" ? 2 : 1;
  }
  return view_time;
}

void Record(const char* reason, int64_t now) {
  if (!Diagnostics()) return;
  const double seconds = window_start && frequency ? double(now - window_start) / double(frequency) : 0.0;
  host::Record("native-d3d-tick", fmt::format(
      "\"reason\":\"{}\",\"iteration\":{},\"owning\":{},\"gate\":{},\"tick\":{},\"period\":{:.6f},\"alpha\":{:.4f},"
      "\"frame_rate\":{},\"pace_rate\":{},\"ticks\":{},\"skipped\":{},\"capped\":{},\"debt_ms\":{:.3f},"
      "\"window_s\":{:.3f},\"window_ticks_per_s\":{:.3f},\"window_iterations_per_s\":{:.3f},\"view_time\":{},"
      "\"window_view_dt_per_s\":{:.4f},\"window_tick_dt_per_s\":{:.4f}",
      reason, frame.index, Owning(), frame.gate, frame.tick, frame.period, frame.alpha, FrameRate(), PaceRate(), frame.ticks,
      frame.skipped, capped, debt * 1e3, seconds, seconds > 0 ? double(window_ticks) / seconds : 0.0,
      seconds > 0 ? double(window_iterations) / seconds : 0.0, ViewTime(),
      seconds > 0 ? window_view_dt / seconds : 0.0, seconds > 0 ? window_tick_dt / seconds : 0.0));
  window_start = now;
  window_ticks = window_iterations = 0;
  window_view_dt = window_tick_dt = 0;
}

// Once per loop iteration, by the first of its three calls.
void Decide(uint8_t* base) {
  if (decided) return;
  decided = true;
  const int64_t now = platform::Ticks();
  if (!frequency) {
    frequency = platform::TickFrequency();
    window_start = now;
  }
  const bool f8 = by_variable && platform::KeyDown(platform::Key::kF8);
  const bool toggled = f8 && !f8_down;
  f8_down = f8;
  if (toggled) switched_off = !switched_off;
  ++frame.index;
  const uint32_t time = Get32(base, kTimePointer);
  const bool fixed = time && Get32(base, time + 72);
  const float period = time ? GetFloat(base, time + 76) : 0.0f;
  const double frame_period = 1.0 / double(FrameRate());
  const bool valid_period = fixed && std::isfinite(period) && period > 1e-4f && period <= 0.25f;
  const bool gate = Owning() && valid_period && period > frame_period * 1.25;
  // Without the gate under the fixed step, every iteration is a tick at the
  // title's step: the limiter paces at the tick rate (PaceRate).
  pace_rate.store(valid_period && !gate ? uint32_t(std::lround(1.0 / double(period))) : FrameRate());
  const bool changed = gate != frame.gate || (gate && std::fabs(period - frame.period) > 1e-6f);
  frame.gate = gate;
  frame.period = gate ? period : 0.0f;
  if (!gate) {
    frame.tick = true;
    frame.alpha = 1.0f;
    started = false;
    view_started = false;
    debt = 0;
    frames_since_tick = 0;
  } else {
    // A new run of the gate starts with a tick; then the real time since the
    // previous iteration is owed. A tick is due within half a frame of its
    // period, so the frames at a steady rate alternate without jitter.
    debt = started ? debt + double(now - last) / double(frequency) : double(period);
    started = true;
    if (debt > 2.0 * period) {
      debt = 2.0 * period;
      ++capped;
    }
    frame.tick = debt >= double(period) - frame_period * 0.5;
    if (frame.tick) {
      debt -= period;
      ++frame.ticks;
      frames_since_tick = 0;
    } else {
      ++frame.skipped;
      ++frames_since_tick;
    }
    // The frames are presented at the limiter's steady rate, not when their
    // iteration starts (the delivery waits for the render thread): the alpha
    // follows the frames since the tick, and the real time only when a frame
    // is more than half a frame off its slot (a slow frame).
    const double real = debt / double(period);
    const double paced = double(frames_since_tick) * frame_period / double(period);
    const double alpha = std::fabs(paced - real) <= 0.5 * frame_period / double(period) ? paced : real;
    frame.alpha = float(std::clamp(alpha, 0.0, 1.0));
  }
  last = now;
  ++window_iterations;
  if (frame.tick) ++window_ticks;
  if (toggled || changed || frame.index % 300 == 0)
    Record(toggled ? "f8" : changed ? "mode" : "periodic", now);
  motion::LoopStart(base);  // REX_NATIVE_MOTION_RECORD.
}

// The view block of 82684518 (8268479C to 82684868) without the sound
// (82668A78 advances by the dt; 82532328 and 8227AC30 after the delivery):
// the arguments and stores of the original, in its order.
void ViewBlock(PPCContext& ctx, uint8_t* base) {
  const uint32_t view = Get32(base, kViews + 12);
  if (!view || Get32(base, kViewsOff)) return;
  auto call = [&](void (*fn)(PPCContext& __restrict, uint8_t*), uint32_t r3, uint32_t r4) {
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    fn(ctx, base);
  };
  call(sub_82684050, view, 0);
  call(sub_8259E908, view, 0);
  const uint32_t world = Get32(base, kViews + 8);
  Put32(base, view + 2248, world);
  if (world) {
    Put32(base, Get32(base, world + 48) + 144, view);
    call(sub_8265A058, Get32(base, view + 2248), view);
  }
  call(sub_826AC2E8, Get32(base, kWorldEffects), view);
  call(sub_8259FF40, view, 1);
}

bool Gating() { return gpu::Active() && Enabled(); }
}  // namespace

bool Enabled() {
  static const bool forced = [] { const char* v = std::getenv("REX_NATIVE_STATE_INTERPOLATION"); return v && std::string_view(v) == "1"; }();
  const uint32_t accepted = configured_rate.load();
  return by_variable ? forced : (accepted ? accepted : CurrentSettings().frame_rate) > 30;
}

bool Owning() { return Enabled() && !switched_off; }

uint32_t FrameRate() {
  static const uint32_t forced = [] {
    const char* text = std::getenv("REX_NATIVE_FRAME_RATE");
    const int n = text ? std::atoi(text) : 0;
    return n >= 30 && n <= 240 ? uint32_t(n) : 0u;
  }();
  const uint32_t accepted = configured_rate.load();
  const uint32_t option = accepted ? accepted : CurrentSettings().frame_rate;
  return forced ? forced : option > 30 ? option : 60u;
}

uint32_t PaceRate() {
  const uint32_t rate = pace_rate.load();
  return rate ? rate : FrameRate();
}

const Frame& Current() { return frame; }

bool InGatedStep() { return in_step; }

bool InMainStep() { return in_main_step; }

void BeginView(uint8_t* base) {
  view_address = 0;
  if (!Gating() || !in_step || !frame.gate || !ViewTime()) return;
  const uint32_t time = Get32(base, kTimePointer);
  if (!time) return;
  // The image's time, in periods: the last tick plus alpha. A new run of the
  // gate starts with a frame's share of the period.
  const double shown = double(frame.ticks) + frame.alpha;
  const double advance = view_started ? shown - view_shown : 1.0 / (double(frame.period) * FrameRate());
  view_started = true;
  view_shown = shown;
  const double factor = ViewTime() == 2 ? (frame.tick ? 1.0 : 0.0) : std::clamp(advance, 0.0, 2.0);
  view_saved = ReadDt(base, time);
  view_set = {uint64_t(int64_t(double(int64_t(view_saved.us)) * factor)), float(view_saved.dt * factor),
              float(view_saved.raw * factor)};
  WriteDt(base, time, view_set);
  view_ahead = ViewTime() == 2 ? 0.0 : double(view_saved.dt) * frame.alpha;
  view_factor = factor;
  view_address = time;
  window_view_dt += view_set.dt;
  window_tick_dt += view_saved.dt;
}

void EndView(uint8_t* base) {
  if (!view_address) return;
  const EngineDt now = ReadDt(base, view_address);
  if (now.us == view_set.us && now.dt == view_set.dt && now.raw == view_set.raw) WriteDt(base, view_address, view_saved);
  view_address = 0;
  view_ahead = 0;
  view_factor = 1;
}

double ViewAhead() { return view_address ? view_ahead : 0.0; }

double ViewFactor() { return view_address ? view_factor : 1.0; }

const char* Label() {
  if (!Enabled() || !by_variable) return "";
  if (switched_off) return "F8: 1/60 s";
  if (!frame.gate) return "F8: ticks (fora)";
  return interpolation::Enabled() ? "F8: interpolado" : "F8: ticks";
}

// The three calls of the main loop.
void MapStep(PPCContext& ctx, uint8_t* base) {
  const uint32_t option = CurrentSettings().frame_rate;
  if (gpu::Active() && configured_rate.load() != option) {
    // 82684A14..82684A20 calls MapStep, WorldChangeStep, MainStep, in that
    // order on every iteration. Keep one pacing mode across all three.
    interpolation::ResetHistory(base);
    started = decided = view_started = false;
    frames_since_tick = 0; last = 0; debt = 0; view_shown = 0; view_time = -1;
    frame.gate = false; frame.alpha = 1; frame.tick = true;
    pace_rate.store(0);
    configured_rate.store(option);
    host::Record("native-live-frame-rate", fmt::format("\"frame_rate\":{},\"clock_reset\":true", option));
    REXLOG_INFO("[narutorise] Frame rate: {} (interpolation clock reset)", option);
  }
  if (Gating()) {
    Decide(base);
    if (!frame.tick) return;
  }
  __imp__sub_82677D20(ctx, base);
}

void WorldChangeStep(PPCContext& ctx, uint8_t* base) {
  if (Gating()) {
    Decide(base);
    if (!frame.tick) return;
  }
  __imp__sub_826843D8(ctx, base);
}

void MainStep(PPCContext& ctx, uint8_t* base) {
  in_main_step = true;
  if (!Gating()) {
    if (gpu::Active()) motion::LoopStart(base);
    __imp__sub_82684518(ctx, base);
    if (gpu::Active()) motion::MainStepDone(base);
    in_main_step = false;
    return;
  }
  Decide(base);
  in_step = frame.gate;
  if (frame.tick) __imp__sub_82684518(ctx, base);
  else ViewBlock(ctx, base);
  in_step = false;
  in_main_step = false;
  decided = false;
  motion::MainStepDone(base);
}
}  // namespace gpu::tick

REX_HOOK_RAW(sub_82677D20) { gpu::tick::MapStep(ctx, base); }
REX_HOOK_RAW(sub_826843D8) { gpu::tick::WorldChangeStep(ctx, base); }
REX_HOOK_RAW(sub_82684518) { gpu::tick::MainStep(ctx, base); }
