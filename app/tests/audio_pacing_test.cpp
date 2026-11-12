// The callback grid and the queue pacing of the native audio (audio/pacing.h).
#include "audio/pacing.h"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace narutorise::audio;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {
int checks = 0;
void Check(bool value, const std::string& message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

void GridKeepsThePeriod() {
  const auto start = Clock::time_point{} + 1h;
  Grid grid(start);
  Check(grid.Next() == start, "The grid starts at its start");
  Check(!grid.Advance(start), "On time, the grid does not restart");
  Check(grid.Next() == start + kFramePeriod, "One period on");
  Check(kFramePeriod == std::chrono::nanoseconds(5'333'333), "256 samples at 48 kHz are 5.333 ms");
  // A little late: the grid keeps its times (the next wake is sooner).
  Check(!grid.Advance(start + kFramePeriod + 40ms), "40 ms late keeps the grid");
  Check(grid.Next() == start + 2 * kFramePeriod, "The grid did not move with the lateness");
  // A stall: a new grid from now instead of a burst of catch-up callbacks.
  const auto late = start + 2 * kFramePeriod + 500ms;
  Check(grid.Advance(late), "500 ms late restarts the grid");
  Check(grid.Next() == late, "The new grid starts now");
}

void PacerFollowsTheQueue() {
  Pacer pacer;
  Check(pacer.Callbacks(-1) == 1, "Unknown queue: one call per period");
  Check(pacer.Target() == kMinimumTarget, "Before any block is seen, the target is three frames");
  Check(pacer.Callbacks(0) == 2, "Empty queue: two calls");
  pacer.After(512);
  Check(pacer.Callbacks(600) == 1, "Some queue: one call");
  pacer.After(856);
  Check(pacer.Callbacks(768) == 0, "At the target: no call");
  pacer.After(768);
  Check(pacer.Callbacks(0) == 2 && pacer.LargestTake() == 768, "A take is the queue it emptied");
  pacer.After(2048);
  Check(pacer.Callbacks(1024) == 1 && pacer.LargestTake() == 1024, "The largest take is the device's block");
  Check(pacer.Target() == 1024 + kMarginSamples, "The target is the block plus two frames");
  pacer.After(1280);
  Check(pacer.Callbacks(1536) == 0, "At the new target: no call");
  pacer.Reset();
  Check(pacer.LargestTake() == 0 && pacer.Target() == kMinimumTarget, "Reset forgets the device");
}

void PacerTellsAStalledDevice() {
  Pacer pacer;
  const auto start = Clock::time_point{} + 1h;
  pacer.After(4096);
  Check(pacer.Callbacks(4096) == 0, "Full");
  Check(!pacer.Stalled(start, 1000ms), "Not stalled at once");
  Check(pacer.Callbacks(4096) == 0 && !pacer.Stalled(start + 500ms, 1000ms), "Not before the limit");
  Check(pacer.Callbacks(4096) == 0 && pacer.Stalled(start + 1000ms, 1000ms), "Unchanged for the limit: stalled");
  // A full queue that moves is a device that takes frames.
  Pacer moving;
  moving.After(4096);
  auto now = start;
  for (int i = 0; i < 400; ++i) {
    const int32_t queued = 4096 - (i % 2) * 4;
    Check(moving.Callbacks(queued) == 0, "Full while it moves");
    Check(!moving.Stalled(now, 1000ms), "A moving queue never stalls");
    moving.After(queued);
    now += kFramePeriod;
  }
  pacer.Reset();
  Check(!pacer.Stalled(start + 5000ms, 1000ms), "Reset clears the stall");
}

// Ten simulated seconds against the two devices measured (blocks of 480 every
// 10 ms, of 1024 every 21.33 ms): the calls follow 48 kHz, the queue never
// runs dry once started, and it stays within two frames of its target.
void SimulatedDevices() {
  struct Case {
    int32_t block;
    std::chrono::nanoseconds every;
  };
  for (const Case device : {Case{480, 10ms}, Case{1024, std::chrono::nanoseconds(21'333'333)}}) {
    Pacer pacer;
    const auto start = Clock::time_point{} + 1h;
    Grid grid(start);
    int64_t queued = 0, callbacks = 0, max_queue = 0, underruns = 0;
    auto next_take = start + device.every;
    auto now = start;
    while (now < start + 10s) {
      now = grid.Next();
      while (next_take <= now) {
        if (now > start + 200ms && queued < device.block) ++underruns;
        queued = std::max<int64_t>(0, queued - device.block);
        next_take += device.every;
      }
      grid.Advance(now);
      const int n = pacer.Callbacks(static_cast<int32_t>(queued));
      callbacks += n;
      queued += n * int64_t(kFrameSamples);
      pacer.After(static_cast<int32_t>(queued));
      max_queue = std::max(max_queue, queued);
    }
    const double expected = 10.0 * kSampleRate / kFrameSamples;
    const double ratio = callbacks / expected;
    char text[160];
    std::snprintf(text, sizeof(text), "block %d: %lld callbacks for %.0f expected, max queue %lld, %lld underruns",
                  device.block, static_cast<long long>(callbacks), expected, static_cast<long long>(max_queue),
                  static_cast<long long>(underruns));
    std::cout << "  " << text << "\n";
    Check(ratio > 0.99 && ratio < 1.01, std::string("The callbacks follow 48 kHz: ") + text);
    Check(underruns == 0, std::string("The queue never runs dry: ") + text);
    Check(max_queue <= pacer.Target() + 2 * int64_t(kFrameSamples), std::string("The queue stays bounded: ") + text);
  }
}
}  // namespace

int main() {
  try {
    GridKeepsThePeriod();
    PacerFollowsTheQueue();
    PacerTellsAStalledDevice();
    SimulatedDevices();
  } catch (const std::exception& error) {
    std::cerr << "FAIL after " << checks << " checks: " << error.what() << "\n";
    return 1;
  }
  std::cout << "PASS: " << checks << " checks\n";
  return 0;
}
