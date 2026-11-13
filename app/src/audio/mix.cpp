// The title's frame to the device's samples (mix.h).
#include "audio/mix.h"
#include "audio/pacing.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace narutorise::audio {
namespace {
inline float Sample(const uint8_t* frame, uint32_t channel, uint32_t sample) {
  uint32_t bits;
  std::memcpy(&bits, frame + (channel * kFrameSamples + sample) * 4, 4);
  return std::bit_cast<float>(std::byteswap(bits));
}
inline float Clamp(float value) {
  if (std::isnan(value)) return 0.0f;
  return std::clamp(value, -1.0f, 1.0f);
}
}  // namespace

bool MixFrame(const uint8_t* frame, float* out, int channels, const MixSettings& settings) {
  if (channels != 2 && channels != 6) return false;
  if (settings.mute) {
    std::fill_n(out, kFrameSamples * channels, 0.0f);
    return true;
  }
  if (channels == 2) {
    const StereoFold& w = settings.stereo;
    const float scale = w.gain;
    for (uint32_t i = 0; i < kFrameSamples; ++i) {
      // Center and LFE land on both sides, the surrounds on their own side.
      const float mid = Sample(frame, 2, i) * w.center + Sample(frame, 3, i) * w.lfe;
      const float left = (Sample(frame, 0, i) + mid + Sample(frame, 4, i) * w.surround) * scale;
      const float right = (Sample(frame, 1, i) + mid + Sample(frame, 5, i) * w.surround) * scale;
      out[i * 2] = Clamp(left);
      out[i * 2 + 1] = Clamp(right);
    }
    return true;
  }
  const float gain = settings.surround_gain;
  for (uint32_t i = 0; i < kFrameSamples; ++i) {
    for (uint32_t c = 0; c < 6; ++c) out[i * 6 + c] = Clamp(Sample(frame, c, i) * gain);
  }
  return true;
}

void ChannelUse::Add(const uint8_t* frame, const StereoFold& w) {
  ++frames;
  std::array<float, 6> local{};
  float fold = 0.0f;
  for (uint32_t i = 0; i < kFrameSamples; ++i) {
    float v[6];
    for (uint32_t c = 0; c < 6; ++c) {
      v[c] = Sample(frame, c, i);
      if (std::isfinite(v[c])) local[c] = std::max(local[c], std::fabs(v[c]));
    }
    const float mid = v[2] * w.center + v[3] * w.lfe;
    const float left = std::fabs((v[0] + mid + v[4] * w.surround) * w.gain);
    const float right = std::fabs((v[1] + mid + v[5] * w.surround) * w.gain);
    if (std::isfinite(left)) fold = std::max(fold, left);
    if (std::isfinite(right)) fold = std::max(fold, right);
  }
  for (size_t c = 0; c < 6; ++c) {
    if (local[c] > 0.0f) ++active[c];
    peak[c] = std::max(peak[c], local[c]);
  }
  if (fold > 1.0f) ++fold_over;
  fold_peak = std::max(fold_peak, fold);
}

void LayoutCheck::Add(const uint8_t* frame) {
  if (plausible + implausible >= kFramesChecked || frames >= kFramesLooked) return;
  ++frames;
  bool any = false, ok = true;
  std::array<float, 6> local{};
  for (uint32_t c = 0; c < 6 && ok; ++c) {
    for (uint32_t i = 0; i < kFrameSamples; ++i) {
      const float v = Sample(frame, c, i);
      if (!std::isfinite(v) || std::fabs(v) > 16.0f) {
        ok = false;
        break;
      }
      if (v != 0.0f) any = true;
      local[c] = std::max(local[c], std::fabs(v));
    }
  }
  if (!ok) {
    ++implausible;
    return;
  }
  if (!any) {
    ++silent;
    return;
  }
  ++plausible;
  for (size_t c = 0; c < 6; ++c) peak[c] = std::max(peak[c], local[c]);
}
}  // namespace narutorise::audio
