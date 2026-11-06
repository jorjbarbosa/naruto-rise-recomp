#pragma once

// The output path of the native audio layer. The title's mixer (DARE)
// registers one render client with XAudio: a callback that mixes one frame of
// 256 samples and submits it. The hooks (hooks.cpp) keep that client from the
// SDK; a guest thread of ours calls the callback on a grid of 256 samples at
// 48 kHz (5.33 ms), as the console does, with the pacing of pacing.h, and the
// submitted frames go through the mix (mix.h) to the SDL3 device (device.h).
//
// audio_layer = "sdk" leaves the client to the SDK's audio system, and a
// change of it moves the client between the layers at a period boundary.
#include <cstdint>

namespace audio {
// After the runtime's setup, before the title runs: the options.
void Initialize();
// The options from their cvars (audio_layer, audio_channels, master_volume),
// on the thread that set them.
void ReloadSettings();
// Before the title's teardown: stops the grid, closes the device and the
// recording. The client stays registered with whoever holds it.
void Shutdown();

// The title's render client: its driver object, callback and context.
struct Client {
  uint32_t driver = 0, callback = 0, context = 0;
};
enum class Owner { kNone, kNative, kSdk };
bool NativeRequested();
// From the hooks: the client the title registered (callback 0 releases it)
// and the layer that holds it.
void Register(const Client& client, Owner owner);
// From the hooks: one frame the title submits to the native layer, on the
// thread of its callback.
void Deliver(const uint8_t* frame);

// The music attenuation (attenuation.h) over the title's music group
// (hooks.cpp): the gain of the music volume the title sets, and the fade's
// step once a tick, true when it moved (the music volume is applied again).
float MusicFade();
bool StepMusicFade();

// In hooks.cpp: runs the title's original registration on the current guest
// thread, so the SDK takes the client (sdk true) or releases it; true when
// the SDK holds it afterwards.
bool MoveSdkClient(const Client& client, bool sdk);
}  // namespace audio
