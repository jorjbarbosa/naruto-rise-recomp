// The mix of the title's frames (mix.h).
#include "audio/mix.h"
#include <algorithm>
#include <bit>
#include <cstring>

namespace audio {
namespace {
float Sample(const uint8_t* frame, uint32_t channel, uint32_t index) {
  uint32_t bits;
  std::memcpy(&bits, frame + (channel * kFrameSamples + index) * 4, 4);
  return std::bit_cast<float>(std::byteswap(bits));
}
}  // namespace

void Mix(const uint8_t* frame, Layout layout, float gain, float* out, const StereoFold& fold) {
  if (layout == Layout::kSurround) {
    for (uint32_t i = 0; i < kFrameSamples; ++i)
      for (uint32_t c = 0; c < kFrameChannels; ++c)
        out[i * kFrameChannels + c] = std::clamp(Sample(frame, c, i) * gain, -1.0f, 1.0f);
    return;
  }
  // The same order of operations as the SDK's fold, for comparable output.
  const float scale = fold.scale * gain;
  for (uint32_t i = 0; i < kFrameSamples; ++i) {
    const float mid = Sample(frame, 2, i) * fold.center + Sample(frame, 3, i) * fold.lfe;
    const float left = (Sample(frame, 0, i) + mid + Sample(frame, 4, i) * fold.surround) * scale;
    const float right = (Sample(frame, 1, i) + mid + Sample(frame, 5, i) * fold.surround) * scale;
    out[i * 2] = std::clamp(left, -1.0f, 1.0f);
    out[i * 2 + 1] = std::clamp(right, -1.0f, 1.0f);
  }
}

void Interleave(const uint8_t* frame, float* out) {
  for (uint32_t i = 0; i < kFrameSamples; ++i)
    for (uint32_t c = 0; c < kFrameChannels; ++c) out[i * kFrameChannels + c] = Sample(frame, c, i);
}

float VolumeGain(double volume) {
  const float v = float(std::clamp(volume, 0.0, 1.0));
  return v * v;
}
}  // namespace audio
