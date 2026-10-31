#pragma once

// The mix of the title's frames for the device (render.h). A frame is the
// kernel's: 6 planar channels of 256 big-endian floats at 48 kHz, in the order
// FL FR C LFE SL SR. The mix makes interleaved floats of it, folded to stereo
// or passed through as 5.1, with the master gain, clamped to [-1, 1].
#include <cstdint>

namespace audio {
constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kFrameSamples = 256;
constexpr uint32_t kFrameChannels = 6;
constexpr uint32_t kFrameBytes = kFrameSamples * kFrameChannels * 4;

enum class Layout { kStereo, kSurround };
inline uint32_t Channels(Layout layout) { return layout == Layout::kSurround ? 6 : 2; }

// The stereo fold starts as the SDK's (center and surround at -3 dB, LFE
// dropped, scaled by 1 / (1 + 0.707)), so the balance players know stays.
struct StereoFold {
  float center = 0.70710678f;
  float surround = 0.70710678f;
  float lfe = 0.0f;
  float scale = 0.58578644f;
};

// out holds kFrameSamples * Channels(layout) floats.
void Mix(const uint8_t* frame, Layout layout, float gain, float* out, const StereoFold& fold = {});
// The 6 channels interleaved and unscaled, for the dump of the frames.
void Interleave(const uint8_t* frame, float* out);
// The gain of a master_volume from 0 to 1: its square, so the slider follows loudness.
float VolumeGain(double volume);
}  // namespace audio
