// Audio output: guest driver frames to an SDL 3 playback device, or to a clock without one.

#include "host/audio.h"

#include "log.h"
#include "settings/settings.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <windows.h>

namespace host
{
    namespace
    {
        constexpr uint32_t kFrameBytes = kAudioChannels * sizeof(float);
        // The queue is kept at no less than this many frames ahead of the device (21 ms).
        constexpr uint32_t kMinLatencyFrames = 1024;
        // Bound on a wait for the device, in case a wake-up is missed.
        constexpr auto kDeviceWaitTimeout = std::chrono::milliseconds(20);
        // A device that has not pulled for this long has stopped (lost and not replaced): the queue
        // is dropped and the clock paces the producer until it pulls again.
        constexpr int64_t kDeviceStallMs = 200;

        SDL_AudioStream* g_stream = nullptr;
        // g_stream once opened, for the gain set from the main thread.
        std::atomic<SDL_AudioStream*> g_gainStream{ nullptr };
        // The window has the focus (SDL reports it when the window is created).
        std::atomic<bool> g_focused{ true };
        uint32_t g_targetFrames = kMinLatencyFrames;

        // Wake-ups from SDL's device thread. Lock order: SDL's stream lock, then g_mutex; the
        // producer never calls SDL while holding g_mutex.
        std::mutex g_mutex;
        std::condition_variable g_pulled;
        uint64_t g_pulls = 0;
        int64_t g_lastPull = 0;

        // Clock used without a working device: when the frames queued so far would have finished
        // playing.
        int64_t g_frequency = 0;
        int64_t g_clockEnd = 0;
        HANDLE g_timer = nullptr;
        // When the device was found stalled (0: it plays). Producer thread only.
        int64_t g_stalledSince = 0;

        int64_t Now()
        {
            LARGE_INTEGER counter;
            QueryPerformanceCounter(&counter);
            return counter.QuadPart;
        }

        int64_t FramesToTicks(uint32_t frames)
        {
            return int64_t(frames) * g_frequency / kAudioRate;
        }

        // SDL calls this on every pull of the device, whether or not the queue has enough.
        void SDLCALL OnDevicePull(void*, SDL_AudioStream*, int, int)
        {
            {
                std::lock_guard lock(g_mutex);
                g_pulls++;
                g_lastPull = Now();
            }
            g_pulled.notify_one();
        }

        uint32_t QueuedFrames()
        {
            int bytes = SDL_GetAudioStreamQueued(g_stream);
            return bytes > 0 ? uint32_t(bytes) / kFrameBytes : 0;
        }

        // The master volume (squared, so 50 sounds about half as loud), and silence while unfocused
        // with audio.mute_in_background. Any thread: SDL takes the stream's lock.
        void ApplyGain()
        {
            SDL_AudioStream* stream = g_gainStream.load();
            if (stream == nullptr)
                return;
            float volume = float(settings::Volume()) / 100.0f;
            float gain = settings::MuteInBackground() && !g_focused.load() ? 0.0f : volume * volume;
            if (!SDL_SetAudioStreamGain(stream, gain))
                LOG_ONCE("error: SDL_SetAudioStreamGain failed: %s", SDL_GetError());
        }

        // The playback device named audio.device, logging the names of all of them; the default
        // device when the setting is empty or names none.
        SDL_AudioDeviceID ChooseDevice()
        {
            const std::string& wanted = settings::AudioDevice();
            SDL_AudioDeviceID chosen = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
            int count = 0;
            SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
            std::string names;
            for (int i = 0; i < count; i++)
            {
                const char* name = SDL_GetAudioDeviceName(devices[i]);
                if (name == nullptr)
                    continue;
                names += names.empty() ? "\"" : ", \"";
                names += name;
                names += "\"";
                if (!wanted.empty() && chosen == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK && wanted == name)
                    chosen = devices[i];
            }
            SDL_free(devices);
            Log("audio: devices: %s", names.empty() ? "none listed" : names.c_str());
            if (!wanted.empty())
            {
                if (chosen != SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK)
                    Log("audio: using \"%s\"", wanted.c_str());
                else
                    Log("audio: no device named \"%s\", using the default", wanted.c_str());
            }
            return chosen;
        }

        void SleepTicks(int64_t ticks)
        {
            LARGE_INTEGER due;
            due.QuadPart = -std::max<int64_t>(ticks * 10'000'000 / g_frequency, 1);
            if (g_timer != nullptr && SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE))
                WaitForSingleObject(g_timer, INFINITE);
            else
                Sleep(1);
        }
    }

    void OpenAudio()
    {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        g_frequency = frequency.QuadPart;
        g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

        if (SDL_WasInit(SDL_INIT_AUDIO) == 0)
        {
            Log("audio: no audio subsystem, running silent");
            return;
        }
        SDL_AudioSpec spec{ SDL_AUDIO_F32, int(kAudioChannels), int(kAudioRate) };
        g_stream = SDL_OpenAudioDeviceStream(ChooseDevice(), &spec, OnDevicePull, nullptr);
        if (g_stream == nullptr)
        {
            Log("error: no audio device, running silent: %s", SDL_GetError());
            return;
        }

        // Keep two device buffers queued, so a pull always finds a full buffer. The device buffer
        // is counted at the device rate, the queue at 48 kHz.
        SDL_AudioSpec deviceSpec{};
        int deviceFrames = 0;
        if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(g_stream), &deviceSpec, &deviceFrames) &&
            deviceFrames > 0 && deviceSpec.freq > 0)
        {
            uint64_t frames = uint64_t(deviceFrames) * 2 * kAudioRate / uint64_t(deviceSpec.freq);
            g_targetFrames = std::max<uint32_t>(kMinLatencyFrames, uint32_t(frames));
        }
        Log("audio: device %d channels at %d Hz, %d-frame buffer, %u frames queued", deviceSpec.channels,
            deviceSpec.freq, deviceFrames, g_targetFrames);

        g_gainStream = g_stream;
        ApplyGain();

        {
            std::lock_guard lock(g_mutex);
            g_lastPull = Now();
        }
        if (!SDL_ResumeAudioStreamDevice(g_stream))
            Log("error: SDL_ResumeAudioStreamDevice failed: %s", SDL_GetError());
    }

    void QueueAudio(const float* samples, uint32_t frames)
    {
        if (g_stream != nullptr && !SDL_PutAudioStreamData(g_stream, samples, int(frames * kFrameBytes)))
            LOG_ONCE("error: SDL_PutAudioStreamData failed: %s", SDL_GetError());
        // The clock runs along, ready for when there is no working device. A late producer starts
        // a new phase instead of catching up.
        g_clockEnd = std::max(g_clockEnd, Now()) + FramesToTicks(frames);
    }

    void WaitForAudioRoom()
    {
        if (g_stream != nullptr)
        {
            for (;;)
            {
                uint64_t pulls;
                int64_t lastPull;
                {
                    std::lock_guard lock(g_mutex);
                    pulls = g_pulls;
                    lastPull = g_lastPull;
                }
                if (g_stalledSince != 0)
                {
                    // Stalled: the clock paces until the device pulls again.
                    if (lastPull > g_stalledSince)
                    {
                        g_stalledSince = 0;
                        continue;
                    }
                    SDL_ClearAudioStream(g_stream);
                    break;
                }
                if (QueuedFrames() < g_targetFrames)
                    return;
                if (Now() - lastPull > kDeviceStallMs * g_frequency / 1000)
                {
                    LOG_ONCE("audio: the device stopped playing, keeping time without it");
                    g_stalledSince = Now();
                    continue;
                }
                std::unique_lock lock(g_mutex);
                g_pulled.wait_for(lock, kDeviceWaitTimeout, [&] { return g_pulls != pulls; });
            }
        }
        int64_t target = FramesToTicks(g_targetFrames);
        for (int64_t ahead = g_clockEnd - Now(); ahead >= target; ahead = g_clockEnd - Now())
            SleepTicks(ahead - target + 1);
    }

    void SetAudioFocus(bool focused)
    {
        g_focused = focused;
        ApplyGain();
    }
}
