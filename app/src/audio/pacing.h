#pragma once

// How many callbacks of the title one period of the render grid runs
// (render.h): one per period of 256 samples; none while the device's queue
// covers its largest block plus two frames (at least eight frames); two while
// the queue is under two frames. A device that takes large blocks at once thus
// never makes the title run its callbacks in a burst.
//
// A device that stops taking samples (lost, or stalled by the system) leaves
// the queue full: after kStallSamples without a take the pacer runs one
// callback per period again, so the title never waits for the audio.
#include <algorithm>
#include <chrono>
#include <cstdint>

namespace audio {
// The deadlines of the grid, one period of 256 samples at 48 kHz apart
// (16/3 ms, kept exact by counting periods). A deadline missed by more than
// 100 ms (a stall, a debugger) starts a new grid instead of a burst.
class Grid {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr auto kMaximumLag = std::chrono::milliseconds(100);
  explicit Grid(Clock::time_point start) : epoch_(start) {}
  Clock::time_point next() const { return epoch_ + std::chrono::nanoseconds(periods_ * 16'000'000 / 3); }
  // After the deadline: moves to the next one; false when the grid restarted at now.
  bool Advance(Clock::time_point now) {
    ++periods_;
    if (now - next() <= kMaximumLag) return true;
    epoch_ = now;
    periods_ = 0;
    return false;
  }

 private:
  Clock::time_point epoch_;
  int64_t periods_ = 0;
};

class Pacer {
 public:
  static constexpr int32_t kFrame = 256;
  static constexpr int32_t kMinimumQueue = 8 * kFrame;
  static constexpr int32_t kLowQueue = 2 * kFrame;
  static constexpr int32_t kStallSamples = 12000;  // 250 ms of the grid

  // The block the device reports (its period); takes larger than it raise the limit.
  void SetDeviceBlock(int32_t samples) { largest_ = std::max(largest_, samples); }
  // queued: the samples in the device's queue, or -1 without a device.
  int Callbacks(int32_t queued) {
    if (queued < 0) {
      last_ = -1;
      return 1;
    }
    if (last_ >= 0 && queued < last_) {
      largest_ = std::max(largest_, last_ - queued);
      untaken_ = 0;
    } else {
      untaken_ += kFrame;
    }
    if (untaken_ >= kStallSamples) return 1;
    return queued >= Limit() ? 0 : queued < kLowQueue ? 2 : 1;
  }
  // The queue the callbacks of the period left.
  void Ran(int32_t queued) { last_ = queued; }
  // A stalled device: the queue the render thread may keep before it clears it.
  bool Stalled() const { return untaken_ >= kStallSamples; }
  int32_t Limit() const { return std::max(kMinimumQueue, largest_ + kLowQueue); }
  int32_t largest_block() const { return largest_; }
  // A new device: its blocks and queue start over.
  void Reset() { largest_ = 0; last_ = -1; untaken_ = 0; }

 private:
  int32_t largest_ = 0, last_ = -1, untaken_ = 0;
};
}  // namespace audio
