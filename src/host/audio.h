#pragma once

#include <cstdint>

// Audio output: the frames of the guest audio driver (6 channels at 48 kHz, float) to the default
// SDL 3 playback device. SDL converts them to the device's layout, so a stereo device gets a
// downmix.
//
// One producer thread (the audio driver) queues frames and waits for room; SDL's device thread
// drains the queue. Without a device, or once the device stops pulling (lost and not replaced),
// the output still keeps time, so the producer is paced the same way and the game runs silent.
namespace host
{
    constexpr uint32_t kAudioRate = 48000;
    // FL, FR, C, LFE, SL, SR: the XAudio order, and SDL's 5.1 order.
    constexpr uint32_t kAudioChannels = 6;

    // Opens the default playback device; SDL_INIT_AUDIO must have been initialized on the main
    // thread (CreateMainWindow). Producer thread, before the other two calls; it may take a while
    // (the device starts up). A failure is logged and leaves the output keeping time only.
    void OpenAudio();
    // Queues interleaved frames of kAudioChannels samples. Producer thread only.
    void QueueAudio(const float* samples, uint32_t frames);
    // Blocks until the output wants more frames: the queue holds less than the target latency.
    // Producer thread only.
    void WaitForAudioRoom();
}
