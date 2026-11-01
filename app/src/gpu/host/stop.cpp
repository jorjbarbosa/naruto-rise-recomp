// The ordered stop of the native mode: one owner per process, the engine
// released on its owner thread, the final records and the exit code.
#include "gpu/host/internal.h"
#include "diagnostics/incident.h"
#include "sentry_reporting.h"
#include "gpu/backend/dispatcher.h"
#include "gpu/common/platform.h"
#include <cstdlib>
#include <optional>
#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/string.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

namespace gpu::host {
// One ordered stop per process. The first host thread to stop owns
// it; any other (the watchdog, a guest thread whose own operation finds the
// dispatcher closed) records its reason once and parks until the owner exits
// the process, so no second release or exit code races the owner's.
// A parked guest thread leaves at the title termination of the ordered
// close (XThread::CheckTitleTermination, the SDK's cooperative exit), so the
// close can join it; any other parked thread stays until the process exits.
namespace {
std::atomic<uint32_t> stop_owner{0};
std::atomic<uint32_t> parked_stops{0};
}  // namespace
void ClaimStop(std::string_view name, PPCContext* ctx) {
  uint32_t owner = 0;
  const uint32_t self = rex::thread::current_thread_system_id();
  if (stop_owner.compare_exchange_strong(owner, self) || owner == self) return;
  {
    std::lock_guard lock(event_mutex);
    Record("stop-parked-behind-owner", name, ctx, owner);
  }
  parked_stops.fetch_add(1);
  for (;;) {
    rex::thread::Sleep(std::chrono::milliseconds(10));
    rex::system::XThread::CheckTitleTermination();
  }
}
// The stop claimed without parking (the UI thread of the close fallback).
bool TryClaimStop() {
  uint32_t owner = 0;
  const uint32_t self = rex::thread::current_thread_system_id();
  return stop_owner.compare_exchange_strong(owner, self) || owner == self;
}
uint32_t ParkedStops() { return parked_stops.load(); }

// The engine part of a stop, shared by Stop and the ordered close:
// arena and engine released on the owner, then the records. The stop record is
// written only by Stop (stop_record). True when everything was released.
bool ReleaseEngineAndRecord(std::string_view name, PPCContext* ctx, uint32_t address, uint32_t value,
                            bool stop_record, std::string* shutdown_evidence) {
  std::string shutdown, failed_shutdown;
  bool shutdown_ok = true;
  {
    std::lock_guard lock(engine_mutex);
    if (native_engine) {
      shutdown_ok = native_engine->Shutdown(shutdown);
      if (shutdown_ok) {
        active_engine.store(nullptr, std::memory_order_release);
        native_engine.reset();
      }
    }
    if (shutdown_evidence) *shutdown_evidence = shutdown;
    if (failed_native_engine) {
      const bool released = failed_native_engine->Shutdown(failed_shutdown);
      if (released) failed_native_engine.reset();
      shutdown_ok = shutdown_ok && released;
    }
  }
  const std::string interrupt = InterruptRegistrationEvidence();
  {
    std::lock_guard lock(event_mutex);
    if (stop_record) Record("stop-before-uncontracted-operation", name, ctx, address, value);
    if (!shutdown.empty()) {
      Write(fmt::format("{{\"sequence\":{},\"kind\":\"native-engine-diagnostic-release\","
          "\"evidence\":{},\"callback\":{},\"argument\":{},\"registered_guest_thread\":{},"
          "\"registered_cpu\":{},\"callback_invoked\":false}}\n", ++sequence, shutdown,
          engine_registration.callback, engine_registration.argument,
          engine_registration.guest_thread, engine_registration.cpu));
    }
    if (!failed_shutdown.empty()) {
      Write(fmt::format("{{\"sequence\":{},\"kind\":\"native-engine-failed-owner-at-stop\","
          "\"evidence\":{}}}\n", ++sequence, failed_shutdown));
    }
    if (!interrupt.empty()) {
      Write(fmt::format("{{\"sequence\":{},\"kind\":\"native-interrupt-registration-at-stop\","
          "\"evidence\":{}}}\n", ++sequence, interrupt));
    }
    if (frame_completion::started.load())
      Write(fmt::format("{{\"sequence\":{},\"kind\":\"native-own-interrupts-at-stop\",\"evidence\":{}}}\n",
                        ++sequence, frame_completion::Summary()));
    Modules();
  }
  return shutdown_ok;
}
bool ReleaseEngineForClose(std::string_view reason) {
  return ReleaseEngineAndRecord(reason, nullptr, 0, 0, false);
}

namespace {
// A numeric field of an own evidence object, as written; empty when absent.
std::string_view EvidenceField(std::string_view json, std::string_view key) {
  const std::string quoted = fmt::format("\"{}\":", key);
  const size_t at = json.find(quoted);
  if (at == std::string_view::npos) return {};
  const size_t begin = at + quoted.size();
  return json.substr(begin, json.find_first_of(",}", begin) - begin);
}
// Without the journal (the product) a stop would end the process with no
// trace: the SDK log gets the reason, the posted operation that latched the
// engine and the device removed reason (both engine shutdown evidences write
// it), and a message box tells the player before the exit.
void ReportProductStop(std::string_view name, int code, bool released, std::string_view posted,
                       std::string_view shutdown) {
  constexpr size_t kEvidenceChars = 1500;
  std::optional<uint32_t> removed;
  if (const auto field = EvidenceField(shutdown, "device_removed_reason"); !field.empty())
    removed = uint32_t(std::strtoul(std::string(field).c_str(), nullptr, 10));
  REXLOG_ERROR("[narutorise] Native renderer stopped: {} (exit {}, engine released {}, device removed reason "
               "{})", name, code, released, removed ? fmt::format("0x{:08X}", *removed) : "not-queried");
  if (!posted.empty())
    REXLOG_ERROR("[narutorise] Native renderer failed operation: {}", posted.substr(0, kEvidenceChars));
  rex::FlushLogging();
  std::string message = fmt::format("The native renderer stopped and the game will close.\n\nReason: {}", name);
  if (removed && int32_t(*removed) < 0)  // A failed HRESULT.
    message += fmt::format("\nThe graphics card stopped responding (0x{:08X}).", *removed);
#ifdef _WIN32
  message += "\n\nThe details are in the log, in the logs folder next to narutorise.exe. "
             "If this happens again, choose the other graphics API in Settings > Display.";
#else
  message += "\n\nThe details are in the log, in the logs folder next to the narutorise executable.";
#endif
  platform::ShowError("narutorise", message);
}
}  // namespace

[[noreturn]] void Stop(std::string_view name, PPCContext* ctx,
                       uint32_t address, uint32_t value, int code) {
  // Independent of the journal/logger and before ownership waits or teardown.
  narutorise::incident::FirstFailure(name);
  narutorise::incident::Event("renderer-stop", "begin", name);
  ClaimStop(name, ctx);
  narutorise::incident::Event("renderer-quiesce", "begin");
  frame_completion::Quiesce();
  narutorise::incident::Event("renderer-quiesce", "end");
  // The posted operation that latched the engine, read before the release.
  std::string posted, shutdown;
  if (!Journaled())
    if (auto* dispatcher = ActiveDispatcher()) dispatcher->PostedFailure(posted);
  narutorise::incident::Event("renderer-release", "begin");
  const bool released = ReleaseEngineAndRecord(name, ctx, address, value, true, &shutdown);
  narutorise::incident::Event("renderer-release", released ? "end" : "failed", shutdown);
  narutorise::reporting::FailureDetails details;
  details.lr = ctx ? ctx->lr : 0;
  if (auto* ts = rex::runtime::ThreadState::Get()) details.guest_thread = ts->thread_id();
  details.address = address;
  details.value = value;
  details.shutdown = shutdown;
  narutorise::reporting::CaptureFailure(std::string(name), posted, details);
  if (!Journaled()) ReportProductStop(name, released ? code : 88, released, posted, shutdown);
  // Diagnostic termination. No guest return, fence, read pointer or callback.
  // The record writer stays locked, so the exit cuts no line of another thread.
  std::lock_guard final_lock(event_mutex);
  WriteStopProvenance(released ? code : 88);
  std::_Exit(released ? code : 88);
}
void Stop(std::string_view reason, PPCContext& ctx, int code) { Stop(reason, &ctx, 0, 0, code); }
void ClaimStop(std::string_view reason, PPCContext& ctx) { ClaimStop(reason, &ctx); }

PPCContext* Context() {
  auto* ts = rex::runtime::ThreadState::Get();
  return ts ? ts->context() : nullptr;
}
}  // namespace gpu::host
