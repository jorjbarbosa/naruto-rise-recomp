// The callback grid and the queue pacing (pacing.h).
#include "audio/pacing.h"

#include <algorithm>

namespace narutorise::audio {
bool Grid::Advance(Clock::time_point now) {
  next_ += kFramePeriod;
  if (now - next_ > kMaximumLag) {
    next_ = now;
    return true;
  }
  return false;
}

int32_t Pacer::Target() const { return std::max(kMinimumTarget, largest_take_ + kMarginSamples); }

int Pacer::Callbacks(int32_t queued) {
  if (queued < 0) {
    full_ = false;
    return 1;
  }
  // The device took this much since the last period: one of its blocks.
  largest_take_ = std::max(largest_take_, previous_ - queued);
  if (queued >= Target()) {
    if (!full_ || queued != full_queue_) full_since_ = {};
    full_ = true;
    full_queue_ = queued;
    return 0;
  }
  full_ = false;
  return queued < static_cast<int32_t>(kFrameSamples) ? 2 : 1;
}

void Pacer::After(int32_t queued) {
  if (queued >= 0) previous_ = queued;
}

bool Pacer::Stalled(std::chrono::steady_clock::time_point now, std::chrono::milliseconds limit) {
  if (!full_) return false;
  if (full_since_ == std::chrono::steady_clock::time_point{}) {
    full_since_ = now;
    return false;
  }
  return now - full_since_ >= limit;
}

void Pacer::Reset() { *this = Pacer(); }
}  // namespace narutorise::audio
