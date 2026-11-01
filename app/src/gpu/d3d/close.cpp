// Ordered close of the D3D boundary. The window close of the
// native mode reaches NarutoriseApp::OnWindowCloseRequested on the UI thread,
// before ReXApp::OnClosing, which calls TerminateTitle and std::_Exit(0). The
// teardown happens there, in order:
//   1. the render thread parks at its next own Swap or draw (ParkForClose); if
//      it has not parked 10 s later, the UI thread claims the stop (fallback)
//      and the render thread, whose fence wait fails once the own interrupt
//      source closes, parks behind it (stop-parked-behind-owner);
//   2. the own interrupt source is quiesced and its thread joined;
//   3. the title is terminated (KernelState::TerminateTitle, the SDK's
//      cooperative exit, which the parked threads honour) and each guest
//      thread is joined, bounded; the ones left are named as stragglers;
//   4. with every guest thread joined, the resources of the registry are
//      released as D750 releases them, then the device services, the inline
//      areas and the device owner;
//   5. the swapchain is detached and the engine released (owner joined).
// Then the close proceeds: OnClosing finds no guest thread and exits with 0.

#include "gpu/d3d/close.h"
#include "gpu/d3d/destruction_order.h"
#include "gpu/diagnostics/diagnostics.h"
#include "gpu/d3d/hooks.h"
#include "gpu/d3d/device.h"
#include "gpu/d3d/resources.h"
#include "gpu/d3d/draw.h"
#include "gpu/diagnostics/census.h"
#include "gpu/shaders/pipelines.h"
#include "gpu/backend/dispatcher.h"
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
#include <rex/thread.h>
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
// REX_NATIVE_CLOSE_TEST=fallback: the render thread takes the close request
// only 12 s after it arrived (TakeStopRequest), so the 10 s fallback runs.
bool CloseTestFallback() {
  static const bool value = [] {
    if (!Diagnostics()) return false;
    const char* text = std::getenv("REX_NATIVE_CLOSE_TEST");
    return text && std::string_view(text) == "fallback";
  }();
  return value;
}
rex::thread::Event* CloseParkedEvent() {
  static const auto event = rex::thread::Event::CreateManualResetEvent(false);
  return event.get();
}
// The render thread at its next own Swap or draw after the close request:
// owns the stop (or parks behind the fallback's owner), touches nothing more
// and leaves at the title termination.
void ParkForClose(PPCContext& ctx) {
  host::ClaimStop("window-close", ctx);
  host::Record("native-close-render-parked", fmt::format(
      "\"lr\":{},\"guest_thread\":{},\"host_tid\":{}", ctx.lr, GuestThread(), rex::thread::current_thread_system_id()));
  CloseParkedEvent()->Set();
  for (;;) {
    rex::thread::Sleep(std::chrono::milliseconds(10));
    rex::system::XThread::CheckTitleTermination();  // The SDK's cooperative exit; does not return.
  }
}
std::string EngineCounts() {
  std::string out;
  auto* dispatcher = host::ActiveDispatcher();
  if (dispatcher) dispatcher->Run([](Engine& engine, std::string& o) { o = engine.ResourceCounts(); return true; },
                                  out);
  return out.empty() ? std::string("null") : out;
}
}  // namespace gpu

namespace gpu {
bool CloseInOrder() {
  if (!Active() || !device_owner || !device_owner->live()) {
    host::Record("native-close-skipped", fmt::format("\"active\":{},\"device\":{}", Active(),
                                                                 device_owner != nullptr));
    return true;
  }
  using Clock = std::chrono::steady_clock;
  const auto t0 = Clock::now();
  auto ms = [&] { return int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count()); };
  host::Record("native-close-request", fmt::format(
      "\"ui_host_tid\":{},\"test_fallback\":{},\"present\":{}", rex::thread::current_thread_system_id(), CloseTestFallback(),
      PresentSummary()));
  {
    std::lock_guard lock(stop_request_mutex);
    stop_request_reason = "window-close";
    stop_request_code = 0;
    stop_request_close = true;
    stop_request_at = Clock::now();
  }
  stop_requested.store(true, std::memory_order_release);
  // 1. The render thread parks; after 10 s the fallback claims the stop. The
  // 10 s are counted by the clock of the records (a wait may return early by
  // less than the timer resolution).
  bool parked = false;
  for (int64_t left = 10000; !parked && left > 0; left = 10000 - ms())
    parked = rex::thread::Wait(CloseParkedEvent(), false, std::chrono::milliseconds(left)) ==
             rex::thread::WaitResult::kSuccess;
  bool fallback = false;
  if (!parked) {
    fallback = true;
    const bool claimed = host::TryClaimStop();
    host::Record("native-close-fallback", fmt::format(
        "\"after_ms\":{},\"claimed\":{},\"parked_stops\":{}", ms(), claimed, host::ParkedStops()));
    if (!claimed) return true;  // Another stop owns the process exit.
  }
  // 2. No more own interrupt deliveries; the source's thread joined.
  host::QuiesceInterrupts();
  std::string interrupts;
  const bool interrupts_joined = host::JoinInterrupts(2000, interrupts);
  // In the fallback the render thread fails its fence wait on the closed
  // source and parks behind the owner.
  if (fallback)
    for (int i = 0; i < 300 && !host::ParkedStops(); ++i) rex::thread::Sleep(std::chrono::milliseconds(10));
  host::Record("native-close-render", fmt::format(
      "\"parked\":{},\"fallback\":{},\"parked_stops\":{},\"interrupts_joined\":{},\"interrupts\":{{{}}},\"after_ms\":{}",
      parked, fallback, host::ParkedStops(), interrupts_joined, interrupts, ms()));
  // The guard census also on a window close (the render thread is parked).
  if (SkipUncoveredDraws()) host::Record("native-d3d-guard-census", GuardCensusSummary());  // The product too.
  // The product has no journal: the session's skipped draws and operations go to the SDK log.
  if (!host::Journaled()) REXLOG_INFO("[narutorise] Native renderer skipped: {{{}}}", GuardCensusSummary());
  REXLOG_INFO("[narutorise] Destruction order: {{{}}}", destruction_order::Summary());
  RecordPipelines();  // The list of a close boot too.
  // 3. The title terminated as the SDK does it, then each guest thread joined.
  auto* kernel = host::Kernel();
  struct Guest {
    rex::system::object_ref<rex::system::XThread> thread;
    uint32_t id;
    std::string name;
  };
  std::vector<Guest> guests;
  if (kernel)
    for (const auto& thread : kernel->object_table()->GetObjectsByType<rex::system::XThread>())
      if (thread->is_guest_thread() && thread->is_running() && thread->thread()) {
        std::string name;
        for (char c : thread->thread()->name()) name += c == '"' || c == '\\' ? '_' : c;
        guests.push_back({thread, thread->thread_id(), name});
      }
  if (kernel) kernel->TerminateTitle();
  std::string joined_threads, straggler_threads;
  uint32_t joined = 0, stragglers = 0;
  const auto deadline = Clock::now() + std::chrono::seconds(3);
  for (const auto& guest : guests) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    const bool done = rex::thread::Wait(guest.thread->thread(), false, std::chrono::milliseconds(std::max<int64_t>(left, 0))) ==
                      rex::thread::WaitResult::kSuccess;
    std::string& list = done ? joined_threads : straggler_threads;
    list += fmt::format("{}{{\"id\":{},\"name\":\"{}\"}}", list.empty() ? "" : ",", guest.id, guest.name);
    ++(done ? joined : stragglers);
  }
  host::Record("native-close-guest-threads", fmt::format(
      "\"terminated\":{},\"guest_threads\":{},\"joined\":{},\"stragglers\":{},\"joined_threads\":[{}],"
      "\"straggler_threads\":[{}],\"after_ms\":{}", kernel != nullptr, guests.size(), joined, stragglers, joined_threads,
      straggler_threads, ms()));
  // 4. The registry resources, the device services and the device owner, only
  // with every guest thread joined (a straggler could still reach them).
  const std::string counts_before = EngineCounts(), registry_before = "{" + ResourceSummary() + "}";
  uint32_t surfaces = 0, textures = 0, failures = 0;
  std::string first_failure = "null";
  const bool owner_released = stragglers == 0;
  if (owner_released) {
    for (const auto kind : {ResourceRegistry::Kind::Surface, ResourceRegistry::Kind::Texture})
      for (const uint32_t key : resources.Keys(kind)) {
        std::string evidence;
        if (ReleaseRegistered(key, evidence)) {
          ++(kind == ResourceRegistry::Kind::Surface ? surfaces : textures);
        } else if (++failures == 1) {
          first_failure = fmt::format("{{\"key\":{},\"engine\":{}}}", key, evidence.empty() ? "null" : evidence);
        }
      }
    ReleaseInlineDraw();
    device_owner->ReleaseServices();  // Waits for the own queue, releases the frontend, writeback and scratch.
    device_owner.reset();
  }
  const std::string counts_registry = EngineCounts();
  // The own resources no registered header owns: pass backings and resolve
  // destinations of headers the title did not create through D210.
  std::string remaining = "null";
  bool remaining_released = false;
  if (owner_released) {
    auto* owner = host::ActiveDispatcher();
    remaining_released = owner && owner->Run([](Engine& engine, std::string& out) {
      return engine.ReleaseRemaining(out); }, remaining);
    if (remaining.empty()) remaining = "null";
  }
  host::Record("native-close-resources", fmt::format(
      "\"owner_released\":{},\"surfaces_released\":{},\"textures_released\":{},\"failures\":{},\"first_failure\":{},"
      "\"registry_before\":{},\"registry_after\":{{{}}},\"engine_before\":{},\"engine_after_registry\":{},"
      "\"remaining_released\":{},\"remaining\":{},\"engine_after\":{},\"after_ms\":{}",
      owner_released, surfaces, textures, failures + (owner_released && !remaining_released), first_failure,
      registry_before, ResourceSummary(), counts_before, counts_registry, remaining_released, remaining, EngineCounts(),
      ms()));
  // 5. The swapchain, then the engine (the owner thread joined).
  std::string detach;
  auto* dispatcher = host::ActiveDispatcher();
  const bool detached = dispatcher && dispatcher->Run([](Engine& engine, std::string& out) {
    return engine.DetachWindow(out); }, detach);
  const std::string counts_after = EngineCounts();
  // The translation stage before the engine goes (Shutdown stops it first and joins its thread).
  const std::string translation = dispatcher ? "{" + dispatcher->StageSummary() + "}" : "null";
  const bool engine_released = host::ReleaseEngineForClose("window-close");
  host::Record("native-close-complete", fmt::format(
      "\"fallback\":{},\"render_parked\":{},\"interrupts_joined\":{},\"guest_threads\":{},\"joined\":{},"
      "\"stragglers\":{},\"owner_released\":{},\"resource_failures\":{},\"detached\":{},\"detach\":{},"
      "\"engine_after_detach\":{},\"translation\":{},\"engine_released\":{},\"after_ms\":{},"
      "\"then\":\"ReXApp::OnClosing: TerminateTitle, std::_Exit(0)\"", fallback, parked, interrupts_joined, guests.size(),
      joined, stragglers, owner_released, failures, detached, detach.empty() ? "null" : detach, counts_after,
      translation, engine_released, ms()));
  return true;
}
}  // namespace gpu
