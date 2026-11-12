#pragma once

// When the title's render driver callback runs (audio/output.h).
//
// What the title needs (out/native-audio-20261111/etapa2/nota-de-desenho-2a.md):
// one call per 256 samples at 48 kHz, each producing exactly one frame
// (sub_8215AEE8 -> sub_82150170 -> sub_82149800). Its mixer runs the voices in
// real time, so calls must follow the clock, not the device's appetite.
//
// The Grid keeps a fixed 5.333 ms grid. After a stall longer than kMaximumLag
// (a breakpoint, a suspended machine, a long load) it starts again from now: a
// burst of catch-up calls would make the mixer render several frames of its
// voices at once.
//
// The Pacer keeps the device's queue just above what the device takes at once.
// Measured devices take 480 or 1024 samples per block; the target is the
// largest block seen plus two frames of margin, never less than three frames.
// Above the target, a period gets no call; below one frame, two calls (the
// device clock may run slightly fast); otherwise one. Without a queue to read
// (the mute device), one call per period. A queue that stays above the target
// without moving for a while is a device that stopped taking frames.
#include <chrono>
#include <cstdint>

namespace narutorise::audio {
constexpr uint32_t kFrameSamples = 256;
constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kFrameChannels = 6;
// 6 channels, one after the other, of 256 big-endian floats each.
constexpr uint32_t kFrameBytes = kFrameSamples * kFrameChannels * 4;
constexpr std::chrono::nanoseconds kFramePeriod{1'000'000'000LL * kFrameSamples / kSampleRate};
constexpr std::chrono::milliseconds kMaximumLag{100};
constexpr int32_t kMarginSamples = 2 * kFrameSamples;
constexpr int32_t kMinimumTarget = 3 * kFrameSamples;

class Grid {
 public:
  using Clock = std::chrono::steady_clock;
  explicit Grid(Clock::time_point start) : next_(start) {}
  Clock::time_point Next() const { return next_; }
  // After waking at `now` for Next(): one period on. True when the grid was so
  // far behind that it started again from `now`.
  bool Advance(Clock::time_point now);

 private:
  Clock::time_point next_;
};

class Pacer {
 public:
  // Calls for this period, given the queue (samples per channel not yet taken
  // by the device) read just before; negative means unknown.
  int Callbacks(int32_t queued);
  // The queue after this period's calls.
  void After(int32_t queued);
  // True when the queue has stayed above the target, unchanged, for `limit`.
  bool Stalled(std::chrono::steady_clock::time_point now, std::chrono::milliseconds limit);
  void Reset();
  int32_t Target() const;
  int32_t LargestTake() const { return largest_take_; }

 private:
  int32_t previous_ = 0;
  int32_t largest_take_ = 0;
  bool full_ = false;
  int32_t full_queue_ = -1;
  std::chrono::steady_clock::time_point full_since_{};
};
}  // namespace narutorise::audio
