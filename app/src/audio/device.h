#pragma once

// The output device of the native audio layer: an SDL3 stream on the default
// playback device, in stereo or 5.1, at 48 kHz. The render thread puts each
// mixed frame into the stream; there is no SDL callback, so the device takes
// from the stream at its own block size. SDL moves a stream on the default
// device to the new default when the device changes.
#include <cstdint>
#include <string>
#include "audio/mix.h"

struct SDL_AudioStream;

namespace audio {
class Device {
 public:
  Device() = default;
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;
  ~Device() { Release(); }

  bool Open(Layout layout);
  void Close();
  // Closes the stream and SDL's audio.
  void Release();
  bool open() const { return stream_ != nullptr; }
  Layout layout() const { return layout_; }
  // The samples per block the device reports, 0 when unknown.
  int32_t block() const { return block_; }
  // The open device, or why the last Open failed.
  const std::string& description() const { return description_; }
  // The samples (per channel) waiting in the stream, -1 without a stream.
  int32_t Queued() const;
  bool Put(const float* samples, uint32_t sample_count);
  void Clear();

 private:
  SDL_AudioStream* stream_ = nullptr;
  bool subsystem_ = false;
  Layout layout_ = Layout::kStereo;
  int32_t block_ = 0;
  std::string description_;
};
}  // namespace audio
