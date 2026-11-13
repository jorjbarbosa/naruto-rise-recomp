// The SDL and mute devices (device.h).
#include "audio/device.h"
#include "audio/pacing.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <fmt/format.h>

namespace narutorise::audio {
namespace {
class MuteOutput final : public Device {
 public:
  MuteOutput() { facts_.kind = "mute"; }
  const DeviceFacts& Facts() const override { return facts_; }
  bool Put(const float*) override { return true; }
  int32_t Queued() override { return -1; }

 private:
  DeviceFacts facts_;
};

class SdlOutput final : public Device {
 public:
  SdlOutput(SDL_AudioStream* stream, DeviceFacts facts) : stream_(stream), facts_(std::move(facts)) {}
  ~SdlOutput() override {
    SDL_DestroyAudioStream(stream_);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
  }
  const DeviceFacts& Facts() const override { return facts_; }
  bool Put(const float* samples) override {
    const int bytes = static_cast<int>(sizeof(float) * kFrameSamples * facts_.channels);
    return SDL_PutAudioStreamData(stream_, samples, bytes);
  }
  int32_t Queued() override {
    const int bytes = SDL_GetAudioStreamQueued(stream_);
    if (bytes < 0) return -1;
    return bytes / static_cast<int>(sizeof(float) * facts_.channels);
  }

 private:
  SDL_AudioStream* stream_;
  DeviceFacts facts_;
};
}  // namespace

std::unique_ptr<Device> OpenSdlDevice(std::string& error) {
  SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");
  SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, "Naruto Rise");
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    error = fmt::format("SDL_InitSubSystem(SDL_INIT_AUDIO): {}", SDL_GetError());
    return nullptr;
  }
  DeviceFacts facts;
  facts.kind = "sdl";
  if (const char* driver = SDL_GetCurrentAudioDriver()) facts.driver = driver;
  SDL_AudioSpec endpoint{};
  int block = 0;
  if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &endpoint, &block)) {
    facts.endpoint_channels = endpoint.channels;
    facts.endpoint_frequency = endpoint.freq;
    facts.block_samples = block;
  }
  // A mono endpoint gets the stereo fold too, and SDL folds it to one channel.
  // Unknown counts as stereo.
  facts.channels = facts.endpoint_channels > 2 ? 6 : 2;
  SDL_AudioSpec spec{};
  spec.format = SDL_AUDIO_F32;
  spec.channels = facts.channels;
  spec.freq = kSampleRate;
  SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  if (!stream) {
    error = fmt::format("SDL_OpenAudioDeviceStream: {}", SDL_GetError());
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return nullptr;
  }
  const SDL_AudioDeviceID device = SDL_GetAudioStreamDevice(stream);
  if (const char* name = device ? SDL_GetAudioDeviceName(device) : nullptr) facts.name = name;
  if (!device || !SDL_ResumeAudioDevice(device)) {
    error = fmt::format("SDL_ResumeAudioDevice: {}", device ? SDL_GetError() : "no device behind the stream");
    SDL_DestroyAudioStream(stream);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return nullptr;
  }
  return std::make_unique<SdlOutput>(stream, std::move(facts));
}

std::unique_ptr<Device> MuteDevice() { return std::make_unique<MuteOutput>(); }

std::string JsonEscape(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    if (ch == '"' || ch == '\\') {
      out += '\\';
      out += ch;
    } else if (static_cast<unsigned char>(ch) < 0x20) {
      out += fmt::format("\\u{:04x}", static_cast<unsigned char>(ch));
    } else {
      out += ch;
    }
  }
  return out;
}

std::string DeviceJson(const DeviceFacts& f) {
  return fmt::format(
      "{{\"kind\":\"{}\",\"name\":\"{}\",\"driver\":\"{}\",\"channels\":{},\"endpoint_channels\":{},"
      "\"endpoint_frequency\":{},\"block_samples\":{}}}",
      JsonEscape(f.kind), JsonEscape(f.name), JsonEscape(f.driver), f.channels, f.endpoint_channels,
      f.endpoint_frequency, f.block_samples);
}
}  // namespace narutorise::audio
