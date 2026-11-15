// XAM: system services of the dashboard (input, users, loader, notifications, content).

#include "kernel/kernel.h"

#include <windows.h>
#include <xinput.h>

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

// ---- Input: controller 0 comes from XInput ----

// XamInputGetCapabilities(user, flags, X_INPUT_CAPABILITIES*)
GUEST_FUNC(XamInputGetCapabilities)
{
    uint32_t user = ctx.r3.u32;
    uint32_t caps = ctx.r5.u32;
    XINPUT_STATE state;
    if (user >= 4 || XInputGetState(user, &state) != ERROR_SUCCESS)
    {
        ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED;
        return;
    }
    memset(mem::Ptr(caps), 0, 20);
    mem::Store8(caps + 0, 1);       // type: gamepad
    mem::Store8(caps + 1, 1);       // subtype: gamepad
    mem::Store16(caps + 4, 0xF3FF); // buttons
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
    XINPUT_STATE state;
    if (user >= 4 || XInputGetState(user, &state) != ERROR_SUCCESS)
    {
        ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED;
        return;
    }
    mem::Store32(out + 0, state.dwPacketNumber);
    mem::Store16(out + 4, state.Gamepad.wButtons);
    mem::Store8(out + 6, state.Gamepad.bLeftTrigger);
    mem::Store8(out + 7, state.Gamepad.bRightTrigger);
    mem::Store16(out + 8, uint16_t(state.Gamepad.sThumbLX));
    mem::Store16(out + 10, uint16_t(state.Gamepad.sThumbLY));
    mem::Store16(out + 12, uint16_t(state.Gamepad.sThumbRX));
    mem::Store16(out + 14, uint16_t(state.Gamepad.sThumbRY));
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// XamInputSetState(user, flags, X_INPUT_VIBRATION*)
GUEST_FUNC(XamInputSetState)
{
    uint32_t user = ctx.r3.u32;
    if (user >= 4)
    {
        ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED;
        return;
    }
    XINPUT_VIBRATION vibration{ mem::Load16(ctx.r5.u32), mem::Load16(ctx.r5.u32 + 2) };
    ctx.r3.u64 = XInputSetState(user, &vibration);
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
    PrintImportSummary();
    Log("XamLoaderLaunchTitle(\"%s\"): the game wants to start another program", mem::String(ctx.r3.u32));
    std::_Exit(0);
}

GUEST_FUNC(XamLoaderTerminateTitle)
{
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
