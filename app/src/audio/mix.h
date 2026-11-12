#pragma once

// The title's frame to what the device takes (audio/output.h).
//
// The frame of the render driver client is 6 channels, one after the other,
// of 256 big-endian floats (front left, front right, center, LFE, surround
// left, surround right). The device takes interleaved native floats: 2
// channels (the stereo fold) or 6 (5.1). Results are clamped to [-1, 1] as a
// safety net only; a value that is not a number becomes silence.
//
// The gains come from what the title sends (nota-de-desenho-2a.md: a 7.7 min
// session of title, menus, a fight and a cutscene, and a 2.7 min fight): all
// six channels carry sound; the center peaked at 1.47 and 1.50, the fronts at
// 0.99, the LFE at 0.59.
// - Stereo: front + 0.707 x center + 0.707 x surround on each side, LFE left
//   out. Unscaled, that fold peaked at 1.35; a gain of 0.65 brings it to 0.88,
//   below the clamp with margin.
// - 5.1: the channels as they come, with one headroom gain on all of them so
//   the center's 1.50 lands below 1.0 instead of being cut: 0.65 (-3.7 dB).
//   One gain for all keeps the title's balance between channels.
#include <array>
#include <cstddef>
#include <cstdint>

namespace narutorise::audio {
struct StereoFold {
  float center = 0.70710678f;
  float surround = 0.70710678f;
  float lfe = 0.0f;
  float gain = 0.65f;
};
struct MixSettings {
  StereoFold stereo;
  float surround_gain = 0.65f;
  bool mute = false;
};

// One frame (kFrameBytes at `frame`) to 256 interleaved samples of `channels`
// (2 or 6) at `out`. Other channel counts write nothing and return false.
bool MixFrame(const uint8_t* frame, float* out, int channels, const MixSettings& settings);

// Whether the frames look like 6 planes of big-endian floats: the check the
// plan asked of the first frames with sound. A frame is plausible when every
// value is finite and within [-16, 16]; anything else, or a frame read with
// the wrong layout, gives huge or non-finite values. It looks at frames until
// kFramesChecked of them had sound, or kFramesLooked in all (about 10 minutes).
struct LayoutCheck {
  static constexpr uint32_t kFramesChecked = 512;
  static constexpr uint32_t kFramesLooked = 112500;
  uint32_t frames = 0;     // Frames looked at.
  uint32_t silent = 0;     // All zero.
  uint32_t plausible = 0;  // With sound, all values plausible.
  uint32_t implausible = 0;
  std::array<float, 6> peak{};  // Largest magnitude per channel among plausible frames.
  void Add(const uint8_t* frame);
};

// Which channels the title uses, over the whole session: frames with sound per
// channel, the largest magnitude per channel, and frames whose stereo fold
// (with the weights in use) went past 1 before the clamp. They decide the
// downmix from what the title sends, not from defaults.
struct ChannelUse {
  uint64_t frames = 0;
  std::array<uint64_t, 6> active{};
  std::array<float, 6> peak{};
  uint64_t fold_over = 0;
  float fold_peak = 0.0f;
  void Add(const uint8_t* frame, const StereoFold& fold);
};
}  // namespace narutorise::audio
