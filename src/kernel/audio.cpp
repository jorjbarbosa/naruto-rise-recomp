// Audio render driver (XAudio*): runs the title's XAudio callback on a driver thread and plays the
// frames it submits.
//
// The XAudio engine registers one client, {callback, context}. Each call to the callback mixes one
// frame (256 samples, 5.33 ms) and submits it with XAudioSubmitRenderDriverFrame before returning,
// so the driver thread calls it whenever the host output has room for more. The callback records
// its calling thread (KPCR+0x100) and blocks on the engine's processing thread, so it must run on a
// guest thread of its own: a system thread.

#include "apu/xma.h"
#include "host/audio.h"
#include "kernel/kernel.h"
#include "loader.h"

#include <bit>
#include <condition_variable>
#include <mutex>

using namespace kernel;

namespace
{
    constexpr uint32_t kFrameSamples = 256;
    // The one client handle; any nonzero value, the engine only hands it back.
    constexpr uint32_t kClientHandle = 0x41554401;
    // Voice categories of XAudioGetVoiceCategoryVolume (XAudio asks for 0 and 1).
    constexpr uint32_t kVoiceCategories = 2;

    struct Client
    {
        uint32_t callback = 0;
        uint32_t context = 0;
        PPCFunc* function = nullptr;
    };

    // Guards everything below.
    std::mutex g_mutex;
    std::condition_variable g_changed;
    Client g_client;          // callback 0: none registered
    bool g_inCallback = false; // the driver thread is inside the callback
    GuestThread* g_driverThread = nullptr; // started on the first registration, runs for good

    // The driver thread: opens the output, then waits for a client and for room in the output and
    // runs one callback.
    void DriverMain(PPCContext& ctx)
    {
        host::OpenAudio();
        for (;;)
        {
            {
                std::unique_lock lock(g_mutex);
                g_changed.wait(lock, [] { return g_client.callback != 0; });
            }
            host::WaitForAudioRoom();
            // The XMA hardware decodes on the same frame clock as the mixer.
            apu::SignalAudioFrame();

            Client client;
            {
                std::lock_guard lock(g_mutex);
                client = g_client;
                if (client.callback == 0)
                    continue;
                g_inCallback = true;
            }
            ctx.r3.u64 = client.context;
            client.function(ctx, mem::g_base);
            {
                std::lock_guard lock(g_mutex);
                g_inCallback = false;
            }
            g_changed.notify_all();
        }
    }
}

// XAudioRegisterRenderDriverClient(callback*, handle*): callback points to {function, context}.
// One client at a time, as XAudio has one renderer.
GUEST_FUNC(XAudioRegisterRenderDriverClient)
{
    uint32_t callbackPtr = ctx.r3.u32;
    uint32_t handlePtr = ctx.r4.u32;
    if (callbackPtr == 0 || handlePtr == 0)
    {
        ctx.r3.u64 = X_STATUS_INVALID_PARAMETER;
        return;
    }
    Client client;
    client.callback = mem::Load32(callbackPtr);
    client.context = mem::Load32(callbackPtr + 4);
    client.function = FindGuestFunction(client.callback);
    if (client.function == nullptr)
    {
        Log("error: XAudioRegisterRenderDriverClient: callback %08X is not a known function", client.callback);
        ctx.r3.u64 = X_STATUS_INVALID_PARAMETER;
        return;
    }

    {
        std::lock_guard lock(g_mutex);
        if (g_client.callback != 0)
        {
            LOG_ONCE("error: XAudioRegisterRenderDriverClient: a client is already registered");
            ctx.r3.u64 = X_STATUS_UNSUCCESSFUL;
            return;
        }
        mem::Store32(handlePtr, kClientHandle);
        g_client = client;
        // The new thread waits for g_mutex before it can see the client.
        if (g_driverThread == nullptr)
            g_driverThread = StartSystemThread("audio driver", DriverMain);
    }
    Log("audio: render driver client %08X (context %08X)", client.callback, client.context);
    g_changed.notify_all();
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// XAudioUnregisterRenderDriverClient(handle): no callback runs after it returns, unless it is
// called from the callback itself.
GUEST_FUNC(XAudioUnregisterRenderDriverClient)
{
    std::unique_lock lock(g_mutex);
    if (ctx.r3.u32 != kClientHandle || g_client.callback == 0)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }
    g_client = {};
    if (CurrentThread() != g_driverThread)
        g_changed.wait(lock, [] { return !g_inCallback; });
    Log("audio: render driver client unregistered");
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// XAudioSubmitRenderDriverFrame(handle, frame): 6 channels of 256 big-endian floats, one channel
// after the other. Called by the callback on the driver thread.
GUEST_FUNC(XAudioSubmitRenderDriverFrame)
{
    if (ctx.r3.u32 != kClientHandle)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }
    uint32_t frame = ctx.r4.u32;
    float samples[kFrameSamples * host::kAudioChannels];
    for (uint32_t channel = 0; channel < host::kAudioChannels; channel++)
    {
        uint32_t source = frame + channel * kFrameSamples * 4;
        for (uint32_t i = 0; i < kFrameSamples; i++)
            samples[i * host::kAudioChannels + channel] = std::bit_cast<float>(mem::Load32(source + i * 4));
    }
    host::QueueAudio(samples, kFrameSamples);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// XAudioGetVoiceCategoryVolumeChangeMask(handle, mask*): the system volumes never change.
GUEST_FUNC(XAudioGetVoiceCategoryVolumeChangeMask)
{
    mem::Store32(ctx.r4.u32, 0);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// XAudioGetVoiceCategoryVolume(category, volume*): full volume.
GUEST_FUNC(XAudioGetVoiceCategoryVolume)
{
    if (ctx.r3.u32 >= kVoiceCategories)
    {
        ctx.r3.u64 = X_STATUS_INVALID_PARAMETER;
        return;
    }
    mem::Store32(ctx.r4.u32, std::bit_cast<uint32_t>(1.0f));
    ctx.r3.u64 = X_STATUS_SUCCESS;
}
