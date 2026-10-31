// The pacing, the grid and the mix of the native audio layer (audio/pacing.h, audio/mix.h).
#include "audio/mix.h"
#include "audio/pacing.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
int checks = 0;
void Check(bool value, const std::string& message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

// A device that takes blocks of `block` samples at 48 kHz, and the grid
// feeding it through the pacer for `periods` periods.
struct Run {
  int32_t min_queue = INT32_MAX, max_queue = 0, underruns = 0, callbacks = 0, doubled = 0, most = 0;
};
Run Simulate(int32_t block, int periods, int stall_from = -1, int stall_to = -1) {
  audio::Pacer pacer;
  pacer.SetDeviceBlock(block);
  Run run;
  int32_t queue = 0, credit = 0;
  for (int p = 0; p < periods; ++p) {
    const bool stalled = p >= stall_from && p < stall_to;
    if (!stalled) {
      credit += 256;
      while (credit >= block) {
        credit -= block;
        if (queue < block && p > 64) ++run.underruns;
        queue -= std::min(queue, block);
      }
    }
    if (p > 64) {
      run.min_queue = std::min(run.min_queue, queue);
      run.max_queue = std::max(run.max_queue, queue);
    }
    const int n = pacer.Callbacks(queue);
    run.callbacks += n;
    run.doubled += n == 2;
    run.most = std::max(run.most, n);
    queue += 256 * n;
    pacer.Ran(queue);
  }
  return run;
}

std::array<uint8_t, audio::kFrameBytes> Frame(const std::array<float, 6>& values) {
  std::array<uint8_t, audio::kFrameBytes> frame{};
  for (uint32_t c = 0; c < 6; ++c)
    for (uint32_t i = 0; i < audio::kFrameSamples; ++i) {
      const uint32_t bits = std::byteswap(std::bit_cast<uint32_t>(values[c]));
      std::memcpy(frame.data() + (c * audio::kFrameSamples + i) * 4, &bits, 4);
    }
  return frame;
}
bool Near(float a, float b) { return std::abs(a - b) < 1e-6f; }
}  // namespace

int main() {
  try {
    using audio::Pacer;
    Pacer none;
    Check(none.Callbacks(-1) == 1 && none.Callbacks(-1) == 1, "Without a device one callback per period");
    Check(none.Limit() == Pacer::kMinimumQueue, "The queue limit is at least eight frames");

    // 10 s of each device; the callbacks match the 48 kHz of the device.
    for (const int32_t block : {480, 1024, 2400, 4096}) {
      const auto run = Simulate(block, 1875);
      const std::string name = std::to_string(block) + "-sample blocks: ";
      Check(run.underruns == 0, name + "no underrun");
      Check(run.most <= 2, name + "at most two callbacks per period");
      Check(run.max_queue <= std::max(Pacer::kMinimumQueue, block + Pacer::kLowQueue) + 2 * 256,
            name + "the queue stays near its limit");
      const int32_t limit = std::max(Pacer::kMinimumQueue, block + Pacer::kLowQueue);
      Check(std::abs(run.callbacks - 1875) <= (limit + Pacer::kLowQueue + block) / 256,
            name + "one callback per period on average");
    }
    // A take larger than the reported block raises the limit.
    Pacer grown;
    grown.SetDeviceBlock(480);
    grown.Callbacks(3000);
    grown.Ran(3000);
    grown.Callbacks(600);
    Check(grown.largest_block() == 2400 && grown.Limit() == 2400 + Pacer::kLowQueue, "The largest take sets the limit");
    Check(grown.Callbacks(100) == 2, "Two callbacks under two frames");

    // A device that stops taking: after 250 ms the grid runs one callback per
    // period again, and the takes bring the pacing back.
    Pacer stall;
    stall.SetDeviceBlock(480);
    int32_t queue = stall.Limit();
    int full = 0;
    for (int p = 0; p < 46; ++p) {
      full += stall.Callbacks(queue) == 0;
      stall.Ran(queue);
    }
    Check(full == 46 && !stall.Stalled(), "A full queue runs no callback before the stall");
    Check(stall.Callbacks(queue) == 1 && stall.Stalled(), "A stalled device gets one callback per period");
    stall.Ran(queue + 256);
    Check(stall.Callbacks(queue - 480) == 1 && !stall.Stalled(), "A take ends the stall");
    const auto resumed = Simulate(480, 1875, 600, 900);
    Check(resumed.most <= 2, "The stall and its end make no burst");

    // The grid: exact 16/3 ms deadlines, and a new grid after a delay over 100 ms.
    using Clock = audio::Grid::Clock;
    const auto start = Clock::time_point{};
    audio::Grid grid(start);
    for (int i = 0; i < 3; ++i) Check(grid.Advance(grid.next()), "A grid on time keeps its deadlines");
    Check(grid.next() - start == std::chrono::nanoseconds(16'000'000), "Three periods are 16 ms");
    const auto late = grid.next() + std::chrono::milliseconds(150);
    Check(!grid.Advance(late) && grid.next() == late, "A delay over 100 ms starts a new grid");
    Check(grid.Advance(late + std::chrono::milliseconds(50)), "A delay under 100 ms keeps the grid");

    // The mix.
    std::array<float, audio::kFrameSamples * 6> out{};
    const auto frame = Frame({0.5f, -0.25f, 0.2f, 0.9f, 0.1f, -0.1f});
    audio::Mix(frame.data(), audio::Layout::kSurround, 1.0f, out.data());
    Check(Near(out[0], 0.5f) && Near(out[1], -0.25f) && Near(out[3], 0.9f) && Near(out[6 * 255 + 5], -0.1f),
          "5.1 passes the six channels through in order");
    audio::Mix(frame.data(), audio::Layout::kStereo, 1.0f, out.data());
    const audio::StereoFold fold;
    const float mid = 0.2f * fold.center + 0.9f * fold.lfe;
    Check(Near(out[0], (0.5f + mid + 0.1f * fold.surround) * fold.scale) &&
              Near(out[1], (-0.25f + mid - 0.1f * fold.surround) * fold.scale),
          "Stereo folds center and surround at -3 dB without the LFE");
    audio::Mix(frame.data(), audio::Layout::kStereo, 0.25f, out.data());
    Check(Near(out[0], (0.5f + mid + 0.1f * fold.surround) * fold.scale * 0.25f), "The gain scales the mix");
    const auto loud = Frame({3.0f, -3.0f, 0, 0, 0, 0});
    audio::Mix(loud.data(), audio::Layout::kSurround, 1.0f, out.data());
    Check(out[0] == 1.0f && out[1] == -1.0f, "The mix clamps to [-1, 1]");
    std::array<float, audio::kFrameSamples * 6> raw{};
    audio::Interleave(frame.data(), raw.data());
    Check(Near(raw[2], 0.2f) && Near(raw[3], 0.9f), "The recording keeps the frame unscaled");
    Check(audio::VolumeGain(1.0) == 1.0f && audio::VolumeGain(0.5) == 0.25f && audio::VolumeGain(-1) == 0.0f &&
              audio::VolumeGain(2) == 1.0f,
          "The master volume's gain is its square, from 0 to 1");
  } catch (const std::exception& error) {
    std::cerr << "FAIL after " << checks << " checks: " << error.what() << "\n";
    return 1;
  }
  std::cout << "PASS: " << checks << " checks\n";
  return 0;
}
