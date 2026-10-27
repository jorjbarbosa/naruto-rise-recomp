// Explicit developer modes. Never called without --incident_test=<mode>.
#include "diagnostics/incident.h"
#include "sentry_reporting.h"
#include "crash_report.h"
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <thread>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace narutorise::incident {
namespace {
[[gnu::noinline]] int Overflow(unsigned depth) {
  volatile unsigned char stack[16384];
  stack[depth % sizeof(stack)] = static_cast<unsigned char>(depth);
  const int next = Overflow(depth + 1);
  return next + stack[depth % sizeof(stack)];
}
[[noreturn]] void InvalidAccess() {
  *reinterpret_cast<volatile unsigned*>(uintptr_t(1)) = 1;
  std::abort();
}
}
bool VerifyIncident(const std::string& mode) {
  if (mode.empty()) return false;
#ifdef _WIN32
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
  Event("developer-verification", "begin", mode);
  if (mode == "abort") std::abort();
  if (mode == "access-violation") InvalidAccess();
  if (mode == "observer-av") { narutorise::crash_report::Install(); InvalidAccess(); }
  if (mode == "worker-av") { std::thread worker(InvalidAccess); worker.join(); }
  if (mode == "illegal") std::raise(SIGILL);
  if (mode == "stack-overflow") return Overflow(1) != 0;
  if (mode == "fast-fail") {
#ifdef _WIN32
    RaiseFailFastException(nullptr, nullptr, 0);
#else
    std::raise(SIGABRT);
#endif
  }
  if (mode == "exit") std::_Exit(77);
  if (mode == "stop" || mode == "shutdown-blocked") {
    FirstFailure("verification-original-gpu-error", "{\"hresult\":2289696773,\"operation\":\"fixture\"}");
    Event("renderer-shutdown", "begin");
    if (mode == "shutdown-blocked") { RequestSnapshot(); std::this_thread::sleep_for(std::chrono::seconds(4)); }
    std::_Exit(88);
  }
  if (mode == "hang") {
    Event("compiler-fixture", "begin", "manual live collection while UI progress continues");
    RequestSnapshot();
    for (int i = 0; i < 40; ++i) { Progress(Role::kUi); std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    Event("compiler-fixture", "end");
  } else if (mode == "automatic-hang") {
    Activity stuck(Role::kTranslator);
    Event("translator-fixture", "begin", "UI progresses while translator activity remains unfinished");
    for (int i = 0; i < 1300; ++i) { Progress(Role::kUi); std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    Event("translator-fixture", "end");
  } else if (mode == "revoke" || mode == "revoke-pending") {
    if (mode == "revoke-pending") {
      narutorise::reporting::Verify("error");
      std::this_thread::sleep_for(std::chrono::milliseconds(750));
    }
    narutorise::reporting::SetConsent(false);
    if (narutorise::reporting::SetConsent(true)) FirstFailure("consent-test", "re-enable incorrectly reused revoked session");
  } else if (mode == "remote-failure") {
    FirstFailure("verification-renderer-failure", "controlled fixture; no GPU failure is being asserted");
    if (!narutorise::reporting::CaptureFailure("verification-renderer-failure", "controlled fixture; no game data")) return false;
  } else if (mode != "normal") return false;
  ExpectedExit("verification-normal");
  narutorise::reporting::Close();
  std::_Exit(0);
}
}  // namespace narutorise::incident
