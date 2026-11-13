#pragma once

// Where the mixed frames go (audio/output.h): the system's default playback
// device through SDL, or the mute device.
//
// The SDL device is opened once. Its format is asked before opening, so the
// stream opens with the channel count it keeps (2 for a mono or stereo
// endpoint, 6 otherwise); there is no close-and-reopen at the start. SDL
// follows a change of the system's default device by itself.
//
// The mute device takes no frames: it stands in when no device opens or the
// device is lost, so the title's callback keeps its clock and nothing reaches
// the title. The SDL device uses the executable's own SDL (the SDK's runtime
// has its own copy, which keeps the window and the SDK's input).
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace narutorise::audio {
struct DeviceFacts {
  std::string kind;     // "sdl" or "mute".
  std::string name;     // The endpoint's name, as the system lists it.
  std::string driver;   // SDL's audio driver ("wasapi", "pulseaudio", ...).
  int endpoint_channels = 0;
  int endpoint_frequency = 0;
  int block_samples = 0;  // What the device takes at once.
  int channels = 0;       // Submitted per sample: 2 or 6; 0 for the mute device.
};

// The facts as one JSON object, for the device's log lines and the counters
// (diagnostics.h).
std::string DeviceJson(const DeviceFacts& facts);
// JSON string content: quotes, backslashes and control characters escaped.
std::string JsonEscape(std::string_view text);

class Device {
 public:
  virtual ~Device() = default;
  virtual const DeviceFacts& Facts() const = 0;
  int Channels() const { return Facts().channels; }
  // 256 interleaved samples of Channels(). False when the device refused them
  // (lost); the mute device takes nothing and never fails.
  virtual bool Put(const float* samples) = 0;
  // Samples per channel submitted and not yet taken, or -1 (unknown).
  virtual int32_t Queued() = 0;
};

// The default playback device, or null with the reason in `error`.
std::unique_ptr<Device> OpenSdlDevice(std::string& error);
std::unique_ptr<Device> MuteDevice();
}  // namespace narutorise::audio
