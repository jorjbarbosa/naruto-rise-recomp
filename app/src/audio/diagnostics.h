#pragma once

// The diagnostics of the native audio layer: the counters of the render grid
// in the log (once a minute and at the end), and the recording of the title's
// frames by REX_NATIVE_AUDIO_DUMP=<file.wav> (6 channels, 32-bit float, 48 kHz,
// as the title delivers them, before the mix), in both layers.
#include <array>
#include <atomic>
#include <cstdint>

namespace audio::diagnostics {
struct Counters {
  std::atomic<uint64_t> periods{0};     // Periods of the grid with a native registration.
  std::atomic<uint64_t> callbacks{0};   // Callbacks of the title run by the grid.
  std::atomic<uint64_t> frames{0};      // Frames the title delivered to the native layer.
  std::atomic<uint64_t> sdk_frames{0};  // Frames the title delivered to the SDK (audio_layer = "sdk").
  std::atomic<uint64_t> skipped{0};     // Periods without a callback: the queue covered the limit.
  std::atomic<uint64_t> doubled{0};     // Periods with two callbacks: the queue ran low.
  std::atomic<uint64_t> underruns{0};   // Periods that found the device's queue empty.
  std::atomic<uint64_t> restarts{0};    // Grid restarts after a delay over 100 ms.
  std::atomic<uint64_t> stalls{0};      // Device stalls (no sample taken for 250 ms).
  // The game's interface sounds played in our menus, by ui_sounds::Sound, and
  // those the game's sound manager refused.
  std::array<std::atomic<uint64_t>, 5> interface_sounds{};
  std::atomic<uint64_t> interface_refused{0};
  std::atomic<int32_t> queue_limit{0};
  std::atomic<int32_t> largest_block{0};
};
Counters& Get();
// The counters in the log; the final report also closes the recording.
void Report(const char* reason);
// One frame of the title (kFrameBytes, as the kernel takes it).
void Record(const uint8_t* frame);
void CloseRecording();
}  // namespace audio::diagnostics
