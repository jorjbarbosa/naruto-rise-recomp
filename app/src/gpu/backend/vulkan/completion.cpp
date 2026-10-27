// The completion service of the Vulkan backend (completion.h).
#include "gpu/backend/vulkan/completion.h"
#include "gpu/backend/failure_channel.h"

#include <fmt/format.h>
#include <rex/thread.h>

namespace gpu::vulkan {
namespace {
using Clock = std::chrono::steady_clock;
// One wait of the service: short, so the stop and the deadline are seen.
constexpr std::chrono::milliseconds kSlice{20};
double Milliseconds(Clock::duration duration) { return std::chrono::duration<double, std::milli>(duration).count(); }
}  // namespace

Completion::Completion(VkDevice device, std::shared_ptr<FailureChannel> failures)
    : device_(device), failures_(std::move(failures)) {}

Completion::~Completion() {
  Stop();
  // The pending fences may still be signaled by the queue: they stay.
  for (VkFence fence : free_) vkDestroyFence(device_, fence, nullptr);
}

void Completion::Start() {
  thread_ = std::thread([this] {
    rex::thread::set_current_thread_name("Native Vulkan Completion");
    Run();
  });
}

bool Completion::Acquire(VkFence& fence, VkResult& result) {
  {
    std::lock_guard lock(mutex_);
    if (!free_.empty()) {
      fence = free_.back();
      free_.pop_back();
      result = VK_SUCCESS;
      return true;
    }
  }
  const VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fence = VK_NULL_HANDLE;
  result = vkCreateFence(device_, &info, nullptr, &fence);
  if (result != VK_SUCCESS) return false;
  std::lock_guard lock(mutex_);
  ++created_;
  return true;
}

void Completion::Return(VkFence fence) {
  std::lock_guard lock(mutex_);
  free_.push_back(fence);
}

void Completion::Submitted(uint64_t sequence, VkFence fence) {
  {
    std::lock_guard lock(mutex_);
    pending_.push_back({sequence, fence, Clock::now()});
    ++submitted_count_;
  }
  submitted_.notify_one();
}

bool Completion::SetEventOnCompletion(uint64_t sequence, FrameEvent event) {
  {
    std::lock_guard lock(mutex_);
    if (completed_.load(std::memory_order_relaxed) < sequence && !failed_) {
      events_.emplace_back(sequence, std::move(event));
      return true;
    }
    ++events_set_;
  }
  event->Set();
  return true;
}

bool Completion::WaitFor(uint64_t sequence, std::chrono::milliseconds budget) {
  std::unique_lock lock(mutex_);
  return progressed_.wait_for(lock, budget, [&] { return completed_.load(std::memory_order_relaxed) >= sequence; });
}

void Completion::Stop() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  submitted_.notify_all();
  if (thread_.joinable()) thread_.join();
}

std::string Completion::Summary() const {
  std::lock_guard lock(mutex_);
  return fmt::format("\"fences_created\":{},\"fences_free\":{},\"submitted\":{},\"completed\":{},"
                     "\"completed_sequence\":{},\"pending\":{},\"slice_timeouts\":{},\"early_timeouts\":{},"
                     "\"error_results\":{},\"events_set\":{},\"max_wait_ms\":{:.3f},\"failed\":{}",
                     created_, free_.size(), submitted_count_, completed_count_, completed_.load(), pending_.size(),
                     slice_timeouts_, early_timeouts_, error_results_, events_set_, max_wait_ms_, failed_);
}

uint64_t Completion::Submissions() const {
  std::lock_guard lock(mutex_);
  return submitted_count_;
}

#if defined(NATIVE_RENDERER_ENGINE_FAULT_TEST)
void Completion::InjectResultForTest(VkResult result) {
  std::lock_guard lock(mutex_);
  inject_ = result;
}
void Completion::SetDeadlineForTest(std::chrono::milliseconds deadline) {
  std::lock_guard lock(mutex_);
  deadline_ = deadline;
}
#endif

void Completion::Run() {
  // A pending submission's time runs from its submission, or from the last
  // progress when it waited behind earlier ones.
  Clock::time_point progress = Clock::now();
  std::unique_lock lock(mutex_);
  for (;;) {
    submitted_.wait(lock, [&] { return stop_ || !pending_.empty(); });
    if (stop_) return;
    const Pending current = pending_.front();
    lock.unlock();
    const auto attempt = Clock::now();
    VkResult result = vkWaitForFences(device_, 1, &current.fence, VK_TRUE,
                                      uint64_t(std::chrono::nanoseconds(kSlice).count()));
    const auto returned = Clock::now();
    lock.lock();
    if (inject_ != VK_SUCCESS) {
      result = inject_;
      inject_ = VK_SUCCESS;
    }
    const auto since = std::max(current.submitted, progress);
    if (result == VK_SUCCESS) {
      // Complete: the fence returns to the pool reset; a fence that does not
      // reset is destroyed instead.
      if (vkResetFences(device_, 1, &current.fence) == VK_SUCCESS) free_.push_back(current.fence);
      else vkDestroyFence(device_, current.fence, nullptr);
      pending_.pop_front();
      completed_.store(current.sequence, std::memory_order_release);
      ++completed_count_;
      max_wait_ms_ = std::max(max_wait_ms_, Milliseconds(returned - since));
      progress = returned;
      std::vector<FrameEvent> ready;
      std::erase_if(events_, [&](const auto& entry) {
        if (entry.first > current.sequence) return false;
        ready.push_back(std::move(entry.second));
        return true;
      });
      events_set_ += ready.size();
      lock.unlock();
      progressed_.notify_all();
      for (const auto& event : ready) event->Set();
      lock.lock();
      continue;
    }
    if (result == VK_TIMEOUT) {
      ++slice_timeouts_;
      if (!failed_ && returned - since > deadline_) {
        Fail(lock, current, "native-vulkan-completion-deadline", result, Milliseconds(returned - since));
        lock.lock();
        continue;
      }
      // A driver that answers before the slice ends would make the wait spin.
      if (const auto rest = kSlice - (returned - attempt); rest.count() > 0) {
        ++early_timeouts_;
        lock.unlock();
        std::this_thread::sleep_for(rest);
        lock.lock();
      }
      continue;
    }
    ++error_results_;
    if (!failed_) Fail(lock, current, "native-vulkan-completion-result", result, Milliseconds(returned - since));
    else lock.unlock();
    // The fence is asked again after a slice: only its success proves the work done.
    std::this_thread::sleep_for(kSlice);
    lock.lock();
  }
}

void Completion::Fail(std::unique_lock<std::mutex>& lock, const Pending& pending, const char* reason, VkResult result,
                      double waited_ms) {
  failed_ = true;
  std::vector<std::pair<uint64_t, FrameEvent>> waiting;
  waiting.swap(events_);
  events_set_ += waiting.size();
  const std::string evidence = fmt::format(
      "{{\"native\":{{\"api\":\"vulkan\",\"call\":\"vkWaitForFences\",\"result\":{}}},\"reason\":\"{}\","
      "\"sequence\":{},\"completed_sequence\":{},\"pending_ms\":{:.3f},\"deadline_ms\":{}}}",
      int32_t(result), reason, pending.sequence, completed_.load(), waited_ms, deadline_.count());
  lock.unlock();
  failures_->Retain({pending.sequence, kSource, evidence});
  for (const auto& [sequence, event] : waiting) event->Set();
}

}  // namespace gpu::vulkan
