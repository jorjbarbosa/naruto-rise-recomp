// XAM: system services of the dashboard (input, users, loader, notifications, content).

#include "kernel/kernel.h"

#include "host/input.h"

using namespace kernel;

namespace kernel
{
    void StoreVideoMode(uint32_t mode);
}

namespace
{
    // XOVERLAPPED: result, length, context, event handle, completion routine, ..., extended error.
    uint32_t CompleteOverlapped(uint32_t overlapped, uint32_t result, uint32_t length = 0)
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

// ---- Users: nobody is signed in ----

GUEST_FUNC(XamUserGetSigninState) { ctx.r3.u64 = 0; }
GUEST_FUNC(XamUserGetXUID) { ctx.r3.u64 = X_ERROR_NO_SUCH_USER; }
GUEST_FUNC(XamUserGetSigninInfo) { ctx.r3.u64 = X_ERROR_NO_SUCH_USER; }
GUEST_FUNC(XamUserGetName) { ctx.r3.u64 = X_ERROR_NO_SUCH_USER; }

GUEST_FUNC(XamUserCheckPrivilege)
{
    if (ctx.r5.u32 != 0)
        mem::Store32(ctx.r5.u32, 0);
    ctx.r3.u64 = X_ERROR_NO_SUCH_USER;
}

// XamUserReadProfileSettings(titleId, user, xuidCount, xuids, settingCount, settingIds, size*, buffer, overlapped)
GUEST_FUNC(XamUserReadProfileSettings) { ctx.r3.u64 = CompleteOverlapped(StackArg32(ctx, 8), X_ERROR_NO_SUCH_USER); }
// XamUserWriteProfileSettings(titleId, user, settingCount, settings, overlapped)
GUEST_FUNC(XamUserWriteProfileSettings) { ctx.r3.u64 = CompleteOverlapped(ctx.r7.u32, X_ERROR_NO_SUCH_USER); }

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

GUEST_FUNC(XamNotifyCreateListener)
{
    auto object = std::make_shared<KernelObject>(ObjectType::Notification);
    ctx.r3.u64 = CreateHandle(object);
}

// XNotifyGetNext(handle, matchId, id*, parameter*): nothing to report.
GUEST_FUNC(XNotifyGetNext) { ctx.r3.u64 = 0; }
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
    host::StopInput();
    PrintImportSummary();
    Log("XamLoaderLaunchTitle(\"%s\"): the game wants to start another program", mem::String(ctx.r3.u32));
    std::_Exit(0);
}

GUEST_FUNC(XamLoaderTerminateTitle)
{
    host::StopInput();
    PrintImportSummary();
    Log("XamLoaderTerminateTitle: the game asked to quit");
    std::_Exit(0);
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

// ---- Content and enumeration: no storage devices yet ----

// XamEnumerate(handle, flags, buffer, size, itemsReturned*, overlapped)
GUEST_FUNC(XamEnumerate)
{
    if (ctx.r7.u32 != 0)
        mem::Store32(ctx.r7.u32, 0);
    ctx.r3.u64 = CompleteOverlapped(ctx.r8.u32, X_ERROR_NO_MORE_FILES);
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
