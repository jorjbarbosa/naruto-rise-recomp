// XAM: system services of the dashboard (input, loader, notifications, messages, memory, network).

#include "kernel/xam.h"

#include "host/input.h"
#include "host/quit.h"
#include "kernel/kernel.h"

#include <deque>
#include <mutex>
#include <utility>
#include <vector>

using namespace kernel;

namespace kernel
{
    void StoreVideoMode(uint32_t mode);

    // XOVERLAPPED: result, length, context, event handle, completion routine, ..., extended error.
    uint32_t CompleteOverlapped(uint32_t overlapped, uint32_t result, uint32_t length)
    {
        if (overlapped == 0)
            return result;
        mem::Store32(overlapped + 0x00, result);
        mem::Store32(overlapped + 0x04, length);
        mem::Store32(overlapped + 0x18, result);
        uint32_t event = mem::Load32(overlapped + 0x0C);
        if (event != 0)
        {
            auto object = ObjectFromHandle(event);
            if (object != nullptr)
                SetEvent(object);
        }
        return X_ERROR_IO_PENDING;
    }
}

GUEST_FUNC(XGetVideoMode) { StoreVideoMode(ctx.r3.u32); }
GUEST_FUNC(XGetAVPack) { ctx.r3.u64 = 6; } // HDMI
GUEST_FUNC(XGetGameRegion) { ctx.r3.u64 = 0xFFFF; }
GUEST_FUNC(XGetLanguage) { ctx.r3.u64 = 1; } // English
GUEST_FUNC(XamGetSystemVersion) { ctx.r3.u64 = (2u << 28) | (17559u << 8); }

// ---- Input: the player slots of host/input (gamepads, keyboard on slot 0) ----
// The game polls users 0-3 through the XDK XInput wrappers (822DD968/70/80, flags 1) and treats any
// result other than X_ERROR_DEVICE_NOT_CONNECTED as a connected pad; GetCapabilities and GetState
// therefore agree for every slot.

// XamInputGetCapabilities(user, flags, X_INPUT_CAPABILITIES*)
GUEST_FUNC(XamInputGetCapabilities)
{
    uint32_t user = ctx.r3.u32;
    uint32_t caps = ctx.r5.u32;
    host::PadState state;
    if (!host::GetPadState(int(user), state))
    {
        ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED;
        return;
    }
    // Type, subtype (gamepad), flags, then the X_INPUT_GAMEPAD and X_INPUT_VIBRATION fields the pad
    // supports, all of them.
    mem::Store8(caps + 0, 1);
    mem::Store8(caps + 1, 1);
    mem::Store16(caps + 2, 0);
    mem::Store16(caps + 4, 0xF3FF);
    mem::Store8(caps + 6, 0xFF);
    mem::Store8(caps + 7, 0xFF);
    for (int i = 0; i < 4; i++)
        mem::Store16(caps + 8 + i * 2, 0xFFC0);
    mem::Store16(caps + 16, 0xFFFF);
    mem::Store16(caps + 18, 0xFFFF);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// XamInputGetState(user, flags, X_INPUT_STATE*)
GUEST_FUNC(XamInputGetState)
{
    uint32_t user = ctx.r3.u32;
    uint32_t out = ctx.r5.u32;
    host::PadState state;
    if (!host::GetPadState(int(user), state))
    {
        ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED;
        return;
    }
    mem::Store32(out + 0, state.packet);
    mem::Store16(out + 4, state.buttons);
    mem::Store8(out + 6, state.leftTrigger);
    mem::Store8(out + 7, state.rightTrigger);
    mem::Store16(out + 8, uint16_t(state.thumbLX));
    mem::Store16(out + 10, uint16_t(state.thumbLY));
    mem::Store16(out + 12, uint16_t(state.thumbRX));
    mem::Store16(out + 14, uint16_t(state.thumbRY));
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// XamInputSetState(user, flags, X_INPUT_VIBRATION*)
GUEST_FUNC(XamInputSetState)
{
    uint32_t user = ctx.r3.u32;
    uint32_t vibration = ctx.r5.u32;
    bool connected = host::SetPadRumble(int(user), mem::Load16(vibration), mem::Load16(vibration + 2));
    ctx.r3.u64 = connected ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

// ---- Notifications ----
// Each listener has its own queue; a notification goes to the listeners whose area mask has the
// bit of its area (XNOTIFY_SYSTEM is bit 0 for area 0).

namespace
{
    struct NotificationListener : KernelObject
    {
        NotificationListener() : KernelObject(ObjectType::Notification) {}

        uint64_t areas = 0;
        std::deque<std::pair<uint32_t, uint32_t>> queue; // (id, parameter), under g_notifyMutex
    };

    std::mutex g_notifyMutex;
    std::vector<std::weak_ptr<NotificationListener>> g_listeners;
}

namespace kernel
{
    void PostNotification(uint32_t id, uint32_t param)
    {
        uint32_t area = (id >> 16) & 0x1F;
        std::lock_guard lock(g_notifyMutex);
        std::erase_if(g_listeners, [](const auto& listener) { return listener.expired(); });
        for (auto& weak : g_listeners)
        {
            auto listener = weak.lock();
            if (listener != nullptr && (listener->areas >> area & 1) != 0)
                listener->queue.emplace_back(id, param);
        }
    }
}

// XamNotifyCreateListener(areas): areas is a 64-bit mask.
GUEST_FUNC(XamNotifyCreateListener)
{
    auto listener = std::make_shared<NotificationListener>();
    listener->areas = ctx.r3.u64;
    {
        std::lock_guard lock(g_notifyMutex);
        g_listeners.push_back(listener);
    }
    ctx.r3.u64 = CreateHandle(listener);
}

// XNotifyGetNext(handle, matchId, id*, parameter*): pops the oldest notification, or the oldest with
// matchId when it is not 0. Returns 1 when one was popped, 0 otherwise.
GUEST_FUNC(XNotifyGetNext)
{
    auto listener = std::dynamic_pointer_cast<NotificationListener>(ObjectFromHandle(ctx.r3.u32));
    uint32_t match = ctx.r4.u32;
    uint32_t idOut = ctx.r5.u32;
    uint32_t paramOut = ctx.r6.u32;
    ctx.r3.u64 = 0;
    if (listener == nullptr)
        return;

    std::lock_guard lock(g_notifyMutex);
    auto it = std::find_if(listener->queue.begin(), listener->queue.end(),
        [match](const auto& entry) { return match == 0 || entry.first == match; });
    if (it == listener->queue.end())
        return;
    if (idOut != 0)
        mem::Store32(idOut, it->first);
    if (paramOut != 0)
        mem::Store32(paramOut, it->second);
    listener->queue.erase(it);
    ctx.r3.u64 = 1;
}

GUEST_FUNC(XNotifyPositionUI) {}

// ---- XMsg: the XMP system music player (app 0xFA) ----
// No user music plays here, so the title always controls background music. The XDK's
// XMPTitleHasPlaybackControl reads the controller and a flag from XMPGetPlaybackController into
// uninitialized locals and grants control only when both are 0; the game polls it every frame and
// pauses its music while control is reported elsewhere.

namespace
{
    constexpr uint32_t kXmpApp = 0xFA;
    constexpr uint32_t kXmpSetVolume = 0x7000C;
    constexpr uint32_t kXmpSetPlaybackController = 0x7001A;
    constexpr uint32_t kXmpGetPlaybackController = 0x7001B;
    constexpr uint32_t kXmpControllerTitle = 0;

    // Requests that only change the system player, which does not exist: done.
    bool IsXmpRequest(uint32_t app, uint32_t message)
    {
        return app == kXmpApp && (message == kXmpSetVolume || message == kXmpSetPlaybackController);
    }
}

// XMsgInProcessCall(app, message, param1, param2)
GUEST_FUNC(XMsgInProcessCall)
{
    uint32_t app = ctx.r3.u32;
    uint32_t message = ctx.r4.u32;
    uint32_t params = ctx.r5.u32;
    if (app == kXmpApp && message == kXmpGetPlaybackController && params != 0)
    {
        // { client, controller*, flag* }
        uint32_t controller = mem::Load32(params + 4);
        uint32_t flag = mem::Load32(params + 8);
        if (controller != 0)
            mem::Store32(controller, kXmpControllerTitle);
        if (flag != 0)
            mem::Store32(flag, 0);
        ctx.r3.u64 = X_STATUS_SUCCESS;
        return;
    }
    LOG_ONCE("unimplemented: XMsgInProcessCall(app %X, message %X)", app, message);
    ctx.r3.u64 = X_STATUS_NOT_IMPLEMENTED;
}

// XMsgStartIORequest(app, message, overlapped, buffer, size)
GUEST_FUNC(XMsgStartIORequest)
{
    if (!IsXmpRequest(ctx.r3.u32, ctx.r4.u32))
    {
        LOG_ONCE("unimplemented: XMsgStartIORequest(app %X, message %X)", ctx.r3.u32, ctx.r4.u32);
        ctx.r3.u64 = X_STATUS_NOT_IMPLEMENTED;
        return;
    }
    ctx.r3.u64 = CompleteOverlapped(ctx.r5.u32, X_ERROR_SUCCESS);
}

// XMsgStartIORequestEx(app, message, overlapped, buffer, size, extra)
GUEST_FUNC(XMsgStartIORequestEx)
{
    if (!IsXmpRequest(ctx.r3.u32, ctx.r4.u32))
    {
        LOG_ONCE("unimplemented: XMsgStartIORequestEx(app %X, message %X)", ctx.r3.u32, ctx.r4.u32);
        ctx.r3.u64 = X_STATUS_NOT_IMPLEMENTED;
        return;
    }
    ctx.r3.u64 = CompleteOverlapped(ctx.r5.u32, X_ERROR_SUCCESS);
}

// ---- Loader ----

GUEST_FUNC(XamLoaderGetLaunchDataSize)
{
    if (ctx.r3.u32 != 0)
        mem::Store32(ctx.r3.u32, 0);
    ctx.r3.u64 = X_ERROR_NOT_FOUND;
}

GUEST_FUNC(XamLoaderGetLaunchData) { ctx.r3.u64 = X_ERROR_NOT_FOUND; }
GUEST_FUNC(XamLoaderSetLaunchData) { ctx.r3.u64 = X_ERROR_SUCCESS; }

GUEST_FUNC(XamLoaderLaunchTitle)
{
    Log("XamLoaderLaunchTitle(\"%s\"): the game wants to start another program", mem::String(ctx.r3.u32));
    host::QuitFromGuest("the game launched another program");
}

GUEST_FUNC(XamLoaderTerminateTitle)
{
    host::QuitFromGuest("XamLoaderTerminateTitle, the game asked to quit");
}

// XamGetExecutionId(XEX_EXECUTION_ID**): points to the execution info of the XEX header.
GUEST_FUNC(XamGetExecutionId)
{
    uint32_t header = g_module.xexHeader;
    uint32_t count = mem::Load32(header + 0x14);
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t entry = header + 0x18 + i * 8;
        if (mem::Load32(entry) == 0x00040006)
        {
            mem::Store32(ctx.r3.u32, header + mem::Load32(entry + 4));
            ctx.r3.u64 = X_ERROR_SUCCESS;
            return;
        }
    }
    ctx.r3.u64 = X_ERROR_NOT_FOUND;
}

// ---- Memory ----

// XamAlloc(flags, size, address*)
GUEST_FUNC(XamAlloc)
{
    uint32_t address = mem::Alloc(mem::Heap::System, ctx.r4.u32);
    mem::Store32(ctx.r5.u32, address);
    ctx.r3.u64 = address != 0 ? X_ERROR_SUCCESS : 8;
}

GUEST_FUNC(XamFree)
{
    mem::Free(ctx.r3.u32);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// ---- Networking: always offline ----

GUEST_FUNC(NetDll_WSAStartup) { ctx.r3.u64 = 0; }
GUEST_FUNC(NetDll_WSACleanup) { ctx.r3.u64 = 0; }
GUEST_FUNC(NetDll_XNetStartup) { ctx.r3.u64 = 0; }
GUEST_FUNC(NetDll_XNetCleanup) { ctx.r3.u64 = 0; }
GUEST_FUNC(NetDll_socket) { ctx.r3.u64 = 0xFFFFFFFF; }
GUEST_FUNC(NetDll_WSAGetLastError) { ctx.r3.u64 = 10050; } // WSAENETDOWN
GUEST_FUNC(NetDll_XNetGetTitleXnAddr) { ctx.r3.u64 = 1; }   // XNET_GET_XNADDR_PENDING
GUEST_FUNC(NetDll_XNetGetConnectStatus) { ctx.r3.u64 = 4; } // XNET_CONNECT_STATUS_LOST
