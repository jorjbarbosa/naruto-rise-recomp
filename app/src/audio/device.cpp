// The SDL3 output device of the native audio layer (device.h).
#include "audio/device.h"
#include <SDL3/SDL.h>
#include <fmt/format.h>

namespace audio {
bool Device::Open(Layout layout) {
  Close();
  if (!subsystem_) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      description_ = fmt::format("SDL audio did not start: {}", SDL_GetError());
      return false;
    }
    subsystem_ = true;
  }
  SDL_AudioSpec spec{SDL_AUDIO_F32, int(Channels(layout)), int(kSampleRate)};
  stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  if (!stream_) {
    description_ = fmt::format("no output device: {}", SDL_GetError());
    return false;
  }
  layout_ = layout;
  const SDL_AudioDeviceID device = SDL_GetAudioStreamDevice(stream_);
  SDL_AudioSpec obtained{};
  int frames = 0;
  if (!SDL_GetAudioDeviceFormat(device, &obtained, &frames)) obtained = spec;
  block_ = frames > 0 ? frames : 0;
  const char* name = SDL_GetAudioDeviceName(device);
  const char* driver = SDL_GetCurrentAudioDriver();
  description_ = fmt::format("'{}' ({}): {} ch, {} Hz, {} samples per block; sending {}", name ? name : "?",
                             driver ? driver : "?", obtained.channels, obtained.freq, block_,
                             layout == Layout::kSurround ? "5.1" : "stereo");
  if (!SDL_ResumeAudioStreamDevice(stream_)) {
    const std::string error = SDL_GetError();
    Close();
    description_ = fmt::format("the device did not start: {}", error);
    return false;
  }
  return true;
}

void Device::Close() {
  if (stream_) SDL_DestroyAudioStream(stream_);
  stream_ = nullptr;
  block_ = 0;
}

void Device::Release() {
  Close();
  if (subsystem_) SDL_QuitSubSystem(SDL_INIT_AUDIO);
  subsystem_ = false;
}

int32_t Device::Queued() const {
  if (!stream_) return -1;
  const int bytes = SDL_GetAudioStreamQueued(stream_);
  return bytes < 0 ? -1 : bytes / int(sizeof(float) * Channels(layout_));
}

bool Device::Put(const float* samples, uint32_t sample_count) {
  return stream_ && SDL_PutAudioStreamData(stream_, samples, int(sample_count * sizeof(float)));
}

void Device::Clear() {
  if (stream_) SDL_ClearAudioStream(stream_);
}
}  // namespace audio
