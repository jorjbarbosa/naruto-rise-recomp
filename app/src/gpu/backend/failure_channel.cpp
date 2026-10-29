// The failure channel of an engine session (failure_channel.h).
#include "gpu/backend/failure_channel.h"
#include "diagnostics/incident.h"

#include <fmt/format.h>

namespace gpu {
namespace {
std::atomic<uint64_t> next_session{1};
}  // namespace

FailureChannel::FailureChannel() : session_(next_session.fetch_add(1, std::memory_order_relaxed)) {}

bool FailureChannel::Retain(Failure failure) {
  std::vector<std::function<void()>> wakers;
  {
    std::lock_guard lock(mutex_);
    if (failed_.load(std::memory_order_relaxed)) {
      ++later_;
      return false;
    }
    first_ = std::move(failure);
    if (first_.evidence.empty()) first_.evidence = "null";
    narutorise::incident::FirstFailure(first_.source, first_.evidence);
    failed_.store(true, std::memory_order_release);
    wakers = wakers_;
  }
  for (const auto& wake : wakers) wake();
  return true;
}

std::optional<FailureChannel::Failure> FailureChannel::First() const {
  std::lock_guard lock(mutex_);
  if (!failed_.load(std::memory_order_relaxed)) return std::nullopt;
  return first_;
}

uint64_t FailureChannel::Later() const {
  std::lock_guard lock(mutex_);
  return later_;
}

void FailureChannel::OnRetained(std::function<void()> wake) {
  std::lock_guard lock(mutex_);
  wakers_.push_back(std::move(wake));
}

std::string FailureChannel::Refusal() const {
  std::lock_guard lock(mutex_);
  return fmt::format("{{\"refused\":true,\"reason\":\"{}-failed\",\"failed_sequence\":{},\"failure\":{}}}",
                     first_.source, first_.sequence, first_.evidence);
}

}  // namespace gpu
