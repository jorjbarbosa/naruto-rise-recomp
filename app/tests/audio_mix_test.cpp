// The frame conversion of the native audio (audio/mix.h): the stereo fold and
// the 5.1 headroom with the gains chosen from the title's measured levels, the
// clamp and the mute.
#include "audio/mix.h"
#include "audio/pacing.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace narutorise::audio;

namespace {
int checks = 0;
void Check(bool value, const std::string& message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}
bool Near(float a, float b, float tolerance = 1e-6f) { return std::fabs(a - b) <= tolerance; }

// A frame as the title writes it: 6 planes of big-endian floats.
struct Frame {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(kFrameBytes);
  void Set(uint32_t channel, uint32_t sample, float value) {
    const uint32_t bits = std::byteswap(std::bit_cast<uint32_t>(value));
    std::memcpy(bytes.data() + (channel * kFrameSamples + sample) * 4, &bits, 4);
  }
};

void Stereo() {
  Frame frame;
  for (uint32_t i = 0; i < kFrameSamples; ++i) {
    frame.Set(0, i, 0.1f);    // FL
    frame.Set(1, i, -0.2f);   // FR
    frame.Set(2, i, 0.3f);    // C
    frame.Set(3, i, 0.4f);    // LFE
    frame.Set(4, i, 0.05f);   // SL
    frame.Set(5, i, -0.05f);  // SR
  }
  std::vector<float> out(kFrameSamples * 2);
  MixSettings settings;
  Check(MixFrame(frame.bytes.data(), out.data(), 2, settings), "Stereo is a channel count");
  const StereoFold w;
  const float left = (0.1f + 0.3f * w.center + 0.05f * w.surround) * w.gain;
  const float right = (-0.2f + 0.3f * w.center - 0.05f * w.surround) * w.gain;
  Check(Near(out[0], left) && Near(out[1], right), "Front, center and surround fold to each side; LFE stays out");
  Check(Near(out[510], left) && Near(out[511], right), "Every sample, interleaved left and right");
}

// The loudest the title went in the measured session (nota-de-desenho-2a.md):
// fronts at 0.99, center at 1.50, LFE at 0.59, surrounds at 0.08 and 0.04.
void MeasuredPeaks() {
  Frame frame;
  const float peaks[6] = {0.99f, 0.99f, 1.50f, 0.59f, 0.08f, 0.04f};
  for (uint32_t c = 0; c < 6; ++c)
    for (uint32_t i = 0; i < kFrameSamples; ++i) frame.Set(c, i, peaks[c]);
  std::vector<float> surround(kFrameSamples * 6);
  MixSettings settings;
  MixFrame(frame.bytes.data(), surround.data(), 6, settings);
  Check(1.35f * StereoFold{}.gain < 0.9f, "The measured stereo fold peak (1.35 before the gain) stays below 0.9");
  Check(surround[2] < 1.0f && surround[2] > 0.95f, "The center's 1.50 lands just below full scale, not cut");
  Check(Near(surround[0] / surround[2], 0.99f / 1.50f, 1e-5f), "One gain for all keeps the balance between channels");
}

void Clamp() {
  Frame frame;
  frame.Set(0, 0, 4.0f);
  frame.Set(1, 0, -4.0f);
  frame.Set(0, 1, std::numeric_limits<float>::quiet_NaN());
  frame.Set(1, 1, std::numeric_limits<float>::infinity());
  std::vector<float> out(kFrameSamples * 6);
  MixFrame(frame.bytes.data(), out.data(), 6, {});
  Check(out[0] == 1.0f && out[1] == -1.0f, "Clamped to [-1, 1]");
  Check(out[6] == 0.0f, "Not a number becomes silence");
  Check(out[7] == 1.0f, "Infinity is clamped");
}

void Mute() {
  Frame frame;
  frame.Set(0, 0, 0.5f);
  std::vector<float> out(kFrameSamples * 2, 9.0f);
  MixSettings settings;
  settings.mute = true;
  MixFrame(frame.bytes.data(), out.data(), 2, settings);
  bool silent = true;
  for (float v : out) silent = silent && v == 0.0f;
  Check(silent, "audio_mute writes silence");
  Check(!MixFrame(frame.bytes.data(), out.data(), 4, {}), "4 channels are refused");
}

void Layout() {
  LayoutCheck check;
  Frame silent;
  check.Add(silent.bytes.data());
  Frame music;
  for (uint32_t c = 0; c < 6; ++c)
    for (uint32_t i = 0; i < kFrameSamples; ++i) music.Set(c, i, 0.5f * std::sin(0.01f * float(i + 3 * c)));
  check.Add(music.bytes.data());
  // The same frame read as little-endian: what a wrong layout looks like.
  Frame swapped = music;
  for (size_t i = 0; i < swapped.bytes.size(); i += 4)
    std::reverse(swapped.bytes.begin() + i, swapped.bytes.begin() + i + 4);
  check.Add(swapped.bytes.data());
  Check(check.frames == 3 && check.silent == 1 && check.plausible == 1 && check.implausible == 1,
        "Silent, plausible and implausible frames are told apart");
  Check(check.peak[0] > 0.4f && check.peak[0] <= 0.5f, "The peak of a plausible frame");
}

}  // namespace

int main() {
  try {
    Stereo();
    MeasuredPeaks();
    Clamp();
    Mute();
    Layout();
  } catch (const std::exception& error) {
    std::cerr << "FAIL after " << checks << " checks: " << error.what() << "\n";
    return 1;
  }
  std::cout << "PASS: " << checks << " checks\n";
  return 0;
}
