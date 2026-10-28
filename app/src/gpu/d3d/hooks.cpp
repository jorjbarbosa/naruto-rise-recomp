// Infrastructure of the hooks of the D3D boundary (d3d/hooks.h)
// and the entry points the host checks first (Active, RequestStop,
// PublishFrameFence).
#include "gpu/d3d/hooks.h"
#include "gpu/d3d/d3d.h"
#include "gpu/backend/dispatcher.h"
#include "gpu/d3d/device.h"
#include "gpu/d3d/resources.h"
#include "gpu/d3d/draw.h"
#include "gpu/shaders/shaders.h"
#include "gpu/textures/textures.h"
#include "gpu/buffers/buffers.h"
#include "gpu/d3d/state.h"
#include "gpu/diagnostics/census.h"
#include "gpu/diagnostics/diagnostics.h"
#include "gpu/shaders/pipelines.h"
#include "gpu/d3d/frame.h"
#include "gpu/diagnostics/measure.h"
#include "gpu/d3d/close.h"
#include "diagnostics/incident.h"
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
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>
#include <rex/system/xmemory.h>


using namespace gpu;

namespace gpu {
std::unique_ptr<DeviceOwner> device_owner;
[[noreturn]] void StopInOrder(PPCContext& ctx, std::string_view reason, int code) {
  // The reason first, before any of the waits below (the owner's claim, the
  // interrupts, the services): a stop that blocks there still leaves its cause.
  narutorise::incident::FirstFailure(reason);
  REXLOG_ERROR("[narutorise] Native renderer stopping: {}", reason);
  rex::FlushLogging();
  host::ClaimStop(reason, ctx);  // Parks a second stopping thread.
  // Any stop on the measurement thread keeps the partial profile, a guard
  // outside the draw census (a resolve) included.
  if (MeasureSwaps() > 0 && OnMeasureThread()) WriteMeasureFrames(true);
  host::QuiesceInterrupts();  // Before the scratch block is released.
  if (device_owner && device_owner->live()) device_owner->RecordScissors();
  host::Record("native-d3d-resources-at-stop", ResourceSummary());
  host::Record("native-d3d-concurrency", ConcurrencySummary());
  // The draws the guard census skipped, by reason.
  if (SkipUncoveredDraws()) host::Record("native-d3d-guard-census", GuardCensusSummary());  // The product too.
  // The product has no journal: the skipped draws and operations go to the
  // SDK log, before the stop's own line (host::Stop).
  if (!host::Journaled()) REXLOG_ERROR("[narutorise] Native renderer skipped: {{{}}}", GuardCensusSummary());
  host::Record("native-d3d-shaders", ShaderRegistrySummary());
  host::Record("native-d3d-state-shadow", StateShadowSummary());
  host::Record("native-d3d-textures", TextureRegistrySummary());
  host::Record("native-d3d-buffers", BufferRegistrySummary());
  // The draw batches, and the pipelines created with their stable
  // identities (also as pipelines.json in REX_NATIVE_FRAME_DIR, the list the
  // precompilation starts from).
  {
    auto* dispatcher = host::ActiveDispatcher();
    if (dispatcher) host::Record("native-d3d-draw-batches", dispatcher->DrawBatchSummary());
    if (dispatcher) host::Record("native-d3d-translation", dispatcher->StageSummary());
    RecordPipelines();
  }
  // The posted operation that latched the engine, which any later
  // operation of the stop's caller found refused.
  {
    std::string posted;
    auto* dispatcher = host::ActiveDispatcher();
    if (dispatcher && dispatcher->PostedFailure(posted))
      host::Record("native-d3d-posted-failure-at-stop", fmt::format("\"evidence\":{}", posted));
  }
  host::Record("native-present-at-stop", "\"present\":" + gpu::PresentSummary());
  ReleaseInlineDraw();
  if (device_owner) device_owner->ReleaseServices();
  host::Stop(reason, ctx, code);
}
// A fence wait for a frame end ends without publication when a posted
// operation failed or the own interrupt source is closing.
bool FenceWaitFailed() {
  std::string evidence;
  auto* dispatcher = host::ActiveDispatcher();
  return !dispatcher || dispatcher->PostedFailure(evidence) || host::InterruptsClosing() ||
         stop_requested.load(std::memory_order_acquire);
}
// A stop caused by a failed posted operation records its sequence,
// how many operations were posted after it and the guest point of the stop.
void RecordPostedFailure(std::string_view where) {
  std::string evidence;
  auto* dispatcher = host::ActiveDispatcher();
  if (dispatcher && dispatcher->PostedFailure(evidence))
    host::Record("native-d3d-posted-failure",
                             fmt::format("\"stop_point\":\"{}\",\"evidence\":{}", where, evidence));
}
void Step(PPCContext& ctx, bool ok) {
  if (!ok) StopInOrder(ctx, device_owner ? device_owner->failure() : "device-owner-missing", 88);
}
// Stop requested by another host thread (the watchdog), taken by the
// render thread at its next own Swap or draw. The close request of the
// window parks the render thread instead (ParkForClose, d3d/close.cpp).
std::mutex stop_request_mutex;
std::string stop_request_reason;
int stop_request_code = 0;
bool stop_request_close = false;
std::chrono::steady_clock::time_point stop_request_at;
std::atomic<bool> stop_requested{false};
void TakeStopRequest(PPCContext& ctx) {
  if (!stop_requested.load(std::memory_order_acquire)) return;
  std::string reason;
  int code = 0;
  bool close = false;
  {
    std::lock_guard lock(stop_request_mutex);
    // REX_NATIVE_CLOSE_TEST=fallback: the close request is taken only 12 s
    // after it arrived, so the 10 s fallback of the close runs first.
    if (stop_request_close && CloseTestFallback() &&
        std::chrono::steady_clock::now() - stop_request_at < std::chrono::seconds(12)) return;
    if (!stop_requested.exchange(false)) return;
    reason = stop_request_reason;
    code = stop_request_code;
    close = stop_request_close;
  }
  host::Record("native-d3d-stop-request-taken", fmt::format(
      "\"reason\":\"{}\",\"code\":{},\"lr\":{},\"close\":{}", reason, code, ctx.lr, close));
  if (MeasureSwaps() > 0) WriteMeasureFrames(true);  // The partial profile, on the render thread.
  if (close) ParkForClose(ctx);
  StopInOrder(ctx, reason, code);
}

// Concurrency monitor: counts every own hook entry per guest thread and
// records entries made while another guest thread is inside an own hook.
// Nesting on the same thread (8212EA18 inside BeginVertices) is not overlap.
// Two locks and a map lookup per hook, every draw included: it runs with
// the diagnostics only. No allocation per call. The counts are
// keyed by the name's literal and merged by name in the summary, and every
// guest thread seen keeps its entry (depth 0 outside), so no key is built and
// no node is inserted or erased at every hook.
class HookConcurrency {
 public:
  void Enter(const char* name, uint32_t thread) {
    std::lock_guard lock(mutex_);
    ++calls_[{name, thread}];
    Inside* own = nullptr;
    for (auto& entry : inside_) {
      if (entry.thread == thread) {
        own = &entry;
      } else if (entry.depth) {
        ++overlaps_;
        if (samples_.size() < 16)
          samples_.push_back(fmt::format("{{\"entering\":\"{}\",\"thread\":{},\"inside\":\"{}\",\"other\":{}}}",
                                         name, thread, entry.name, entry.thread));
      }
    }
    if (!own) own = &inside_.emplace_back(Inside{thread});
    if (!own->depth++) own->name = name;
  }
  void Leave(uint32_t thread) {
    std::lock_guard lock(mutex_);
    for (auto& entry : inside_)
      if (entry.thread == thread) {
        if (entry.depth) --entry.depth;
        return;
      }
  }
  std::string Summary() {
    std::lock_guard lock(mutex_);
    std::map<std::pair<std::string, uint32_t>, uint64_t> merged;
    for (const auto& [key, count] : calls_) merged[{key.first, key.second}] += count;
    std::string calls;
    for (const auto& [key, count] : merged)
      calls += fmt::format("{}{{\"hook\":\"{}\",\"thread\":{},\"calls\":{}}}", calls.empty() ? "" : ",",
                           key.first, key.second, count);
    return fmt::format("\"monitored\":{},\"calls\":[{}],\"overlaps\":{},\"overlap_samples\":[{}]", Diagnostics(),
                       calls, overlaps_, fmt::join(samples_, ","));
  }
 private:
  struct Inside { uint32_t thread = 0; const char* name = ""; uint32_t depth = 0; };
  struct CallKeyHash {
    size_t operator()(const std::pair<const char*, uint32_t>& k) const {
      return std::hash<const void*>()(k.first) ^ (size_t(k.second) * 0x9E3779B97F4A7C15ull);
    }
  };
  std::mutex mutex_;
  std::unordered_map<std::pair<const char*, uint32_t>, uint64_t, CallKeyHash> calls_;
  std::vector<Inside> inside_;
  uint64_t overlaps_ = 0;
  std::vector<std::string> samples_;
};
HookConcurrency concurrency;
std::string ConcurrencySummary() { return concurrency.Summary(); }
HookScope::HookScope(const char* name) {
  if (!Diagnostics()) return;
  auto* ts = rex::runtime::ThreadState::Get();
  thread_ = ts ? ts->thread_id() : 0;
  concurrency.Enter(name, thread_);
}
HookScope::~HookScope() {
  if (Diagnostics()) concurrency.Leave(thread_);
}
}  // namespace gpu

namespace gpu {
bool Active() { return host::Enabled(); }
bool RequestStop(std::string_view reason, int code) {
  if (!Active() || !device_owner || !device_owner->live()) return false;
  {
    std::lock_guard lock(stop_request_mutex);
    stop_request_reason = reason;
    stop_request_code = code;
  }
  stop_requested.store(true, std::memory_order_release);
  host::Record("native-d3d-stop-request", fmt::format("\"reason\":\"{}\",\"code\":{}", reason, code));
  return true;
}
FramePublication PublishFrameFence(uint32_t fence, FailureChannel& failures) {
  return device_owner ? device_owner->PublishFrame(fence, failures) : FramePublication::kFailed;
}
}  // namespace gpu
