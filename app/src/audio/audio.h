#pragma once

// The port's own audio (docs/plans, audio nativo): a layer at the kernel
// boundary in place of the SDK's audio. The title is not changed.
//
// audio_backend in narutorise.toml chooses it at the start: "native" (the
// default) or "sdk" (the SDK's audio, as before). NARUTORISE_AUDIO_BACKEND in
// the environment overrides it, for the tests. A change needs a restart.
//
// Native: the render driver client (output.h), the voice category volume
// imports and the XMA (xma_runtime.h: the audio system with the XMA registers
// and contexts, and the XMA* exports). "sdk" keeps all of it in the SDK.
#include <string>

namespace rex {
struct RuntimeConfig;
}

namespace narutorise::audio {
// At the start (OnPreSetup), before the title runs: reads the choice, logs it
// and puts it in the crash reports' facts.
void Configure();
// The backend in use. Before Configure, native.
bool Native();
// In OnPreSetup, after Configure: the audio system the runtime creates.
void ConfigureRuntime(rex::RuntimeConfig& config);
const char* BackendName();
// The window is closing: from here on the title winds down (audio counters
// tell it apart from play).
void Closing();
// At the window's close, before the hard exit: stops the callback thread,
// releases the device and logs the counters.
void Shutdown();
// The counters, one JSON object (diagnostics.h).
std::string Summary();
}  // namespace narutorise::audio
