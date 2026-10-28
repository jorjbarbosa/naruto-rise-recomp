#pragma once

// Infrastructure shared by the hooks of the D3D boundary: the guest device
// owner of the live device, the ordered stop with its final records, the step
// check of the device owner, the stop that another host thread requests (the
// watchdog, the window close) and the hook scope of the concurrency monitor.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <rex/ppc/context.h>

namespace gpu {
class DeviceOwner;
extern std::unique_ptr<DeviceOwner> device_owner;
[[noreturn]] void StopInOrder(PPCContext& ctx, std::string_view reason, int code);
// A fence wait for a frame end ends without publication when a posted
// operation failed or the own interrupt source is closing.
bool FenceWaitFailed();
// Records the failed posted operation of a stop.
void RecordPostedFailure(std::string_view where);
void Step(PPCContext& ctx, bool ok);
// Stop requested by another host thread, taken by the render thread at
// its next own Swap or draw (TakeStopRequest); also the window close.
extern std::mutex stop_request_mutex;
extern std::string stop_request_reason;
extern int stop_request_code;
extern bool stop_request_close;
extern std::chrono::steady_clock::time_point stop_request_at;
extern std::atomic<bool> stop_requested;
void TakeStopRequest(PPCContext& ctx);
// Every own hook entry, counted per guest thread by the concurrency monitor
// (with the diagnostics only; off, the scope does nothing).
class HookScope {
 public:
  explicit HookScope(const char* name);
  ~HookScope();
  HookScope(const HookScope&) = delete;
  HookScope& operator=(const HookScope&) = delete;
 private:
  uint32_t thread_ = 0;
};
std::string ConcurrencySummary();
}  // namespace gpu
