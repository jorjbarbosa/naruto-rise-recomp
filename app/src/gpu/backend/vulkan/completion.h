// The completion of the Vulkan backend's submissions (plan V3): a pool of
// binary fences, one per submission, and a service thread that waits for them
// in submission order. Each submission has a sequence, one above the previous;
// the service advances the completed sequence only on a fence's own success,
// and sets the host events of CloseFrame once their sequence completed.
//
// The service waits in short slices, so it sees the stop and the deadline. A
// slice that times out is not a failure; a submission pending beyond the
// deadline is. A failure (an error result, or the deadline) is retained in the
// session's failure channel with its native result before anything wakes: the
// channel's wakers run, then the pending CloseFrame events are set, so a host
// woken by one finds the failure. The service keeps observing the fences after
// a failure: a fence that completes later still advances the completed
// sequence (the resources of its submission can then be released), and one
// that never completes keeps Drain from proving the queue idle.
//
// The fences come from the pool and return to it once complete and reset. The
// owner thread acquires them and records the submissions; Submitted runs
// inside the authorized submission (FailureChannel::Authorize), so the service
// knows every fence the queue may signal. The service takes the channel's lock
// (Retain) only outside its own.
#pragma once

#include <plume_vulkan.h>  // Vulkan through volk, with the platform defines of Plume.

#include "gpu/common/frame_event.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gpu {
class FailureChannel;
}

namespace gpu::vulkan {

class Completion {
 public:
  // The source of the service's failures in the channel.
  static constexpr const char* kSource = "vulkan-completion";
  // A submission pending for longer is a failure.
  static constexpr std::chrono::milliseconds kDeadline{5000};

  Completion(VkDevice device, std::shared_ptr<FailureChannel> failures);
  ~Completion();
  Completion(const Completion&) = delete;
  Completion& operator=(const Completion&) = delete;

  // Starts the service thread. Owner, once.
  void Start();
  // Owner: an unsignaled fence for the next submission, from the pool or new.
  // False with the native result when none could be created.
  bool Acquire(VkFence& fence, VkResult& result);
  // Owner: a fence acquired but not submitted goes back to the pool.
  void Return(VkFence fence);
  // Inside the authorized submission, after vkQueueSubmit returned success:
  // fence signals sequence, which follows the previous one.
  void Submitted(uint64_t sequence, VkFence fence);
  // The highest sequence such that it and every one before completed. Any thread.
  uint64_t Completed() const { return completed_.load(std::memory_order_acquire); }
  // Owner: sets event once sequence completed, at once when it has or when the
  // service already retained a failure; otherwise when it completes, or after
  // the service retains one.
  bool SetEventOnCompletion(uint64_t sequence, FrameEvent event);
  // Owner: waits until sequence completed or the budget ends; true when it
  // completed. A failure does not end the wait: completion is a fact of the
  // fence, and only it proves the work done.
  bool WaitFor(uint64_t sequence, std::chrono::milliseconds budget);
  // Stops and joins the service thread. The fences still pending stay alive
  // (their submission is unproved); the completed ones are destroyed.
  void Stop();
  // The service's counts, as JSON fields.
  std::string Summary() const;
  // The submissions recorded by Submitted. Any thread.
  uint64_t Submissions() const;
#if defined(NATIVE_RENDERER_ENGINE_FAULT_TEST)
  // Host tests: the next wait of the service reports result (once) instead of
  // its own; the deadline of the pending submissions.
  void InjectResultForTest(VkResult result);
  void SetDeadlineForTest(std::chrono::milliseconds deadline);
#endif

 private:
  struct Pending {
    uint64_t sequence = 0;
    VkFence fence = VK_NULL_HANDLE;
    std::chrono::steady_clock::time_point submitted;
  };
  void Run();
  // The first failure of the service: marks it, unlocks lock (mutex_), retains
  // the failure about pending with its native result, then sets every waiting
  // event. Returns with lock unlocked.
  void Fail(std::unique_lock<std::mutex>& lock, const Pending& pending, const char* reason, VkResult result,
            double waited_ms);

  const VkDevice device_;
  const std::shared_ptr<FailureChannel> failures_;
  mutable std::mutex mutex_;
  std::condition_variable submitted_;  // A submission arrived, or the stop.
  std::condition_variable progressed_;  // The completed sequence advanced.
  std::deque<Pending> pending_;
  std::vector<VkFence> free_;
  std::vector<std::pair<uint64_t, FrameEvent>> events_;
  std::atomic<uint64_t> completed_{0};
  bool stop_ = false, failed_ = false;
  std::thread thread_;
  std::chrono::milliseconds deadline_ = kDeadline;
  VkResult inject_ = VK_SUCCESS;
  // Counts (mutex_).
  uint64_t created_ = 0, submitted_count_ = 0, completed_count_ = 0, slice_timeouts_ = 0, early_timeouts_ = 0;
  uint64_t error_results_ = 0, events_set_ = 0;
  double max_wait_ms_ = 0;
};

}  // namespace gpu::vulkan
