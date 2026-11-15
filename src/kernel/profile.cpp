// The signed-in profile: user 0 signed in locally for the whole run, and its profile settings.

#include "kernel/kernel.h"
#include "kernel/xam.h"

#include <fstream>
#include <map>
#include <mutex>
#include <vector>

namespace fs = std::filesystem;
using namespace kernel;

namespace
{
    constexpr uint32_t kSigninStateLocal = 1; // eXUserSigninState_SignedInLocally
    // HRESULT_FROM_WIN32(ERROR_NO_SUCH_USER): XamUserGetXUID and XamUserGetSigninInfo return
    // HRESULTs (their XDK wrappers turn facility 7 back into the Win32 code).
    constexpr uint32_t kHresultNoSuchUser = 0x80070525;
    constexpr uint32_t kHresultInvalidArgument = 0x80070057;
    constexpr uint32_t kSigninOnlineXuidOnly = 1; // XUSER_GET_SIGNIN_INFO_ONLINE_XUID_ONLY
    constexpr uint32_t kUserNameSize = 16;        // XUSER_NAME_SIZE

    // XUSER_PROFILE_SOURCE.
    constexpr uint32_t kSourceNoValue = 0;
    constexpr uint32_t kSourceDefault = 1;
    constexpr uint32_t kSourceTitle = 2;

    // XUSER_DATA_TYPE.
    constexpr uint8_t kTypeUnicode = 4;
    constexpr uint8_t kTypeBinary = 6;

    // XUSER_PROFILE_SETTING (40 bytes): source, user index or XUID at +8, setting id at +16, then
    // XUSER_DATA: type at +24, value at +32 (for unicode and binary: size at +32, pointer at +36).
    // The read result is { count, settings* } followed here by the settings and their payloads.
    constexpr uint32_t kSettingSize = 40;
    constexpr uint32_t kResultHeaderSize = 8;

    // The system settings the game reads (CXeProfile::UpdateProfileSettings), with the XDK
    // defaults: Y axis not inverted, vibration on (3), normal difficulty.
    constexpr uint32_t kYAxisInversion = 0x10040002;
    constexpr uint32_t kControllerVibration = 0x10040003;
    constexpr uint32_t kGamerDifficulty = 0x10040015;
    constexpr uint32_t kVibrationOn = 3;

    // XPROFILEID(type, size, id): type in bits 28-31, maximum size in bits 16-27.
    uint8_t SettingType(uint32_t id) { return uint8_t(id >> 28); }
    uint32_t SettingMaxSize(uint32_t id) { return (id >> 16) & 0xFFF; }
    bool HasPayload(uint32_t id) { return SettingType(id) == kTypeUnicode || SettingType(id) == kTypeBinary; }

    bool DefaultSetting(uint32_t id, uint32_t& value)
    {
        switch (id)
        {
        case kYAxisInversion: value = 0; return true;
        case kControllerVibration: value = kVibrationOn; return true;
        case kGamerDifficulty: value = 0; return true;
        default: return false;
        }
    }

    // Settings the title wrote, by id. Numeric values keep the 8 bytes of the XUSER_DATA value as
    // the guest stores them (big-endian); unicode and binary values keep their payload. Kept in
    // <user>/profile/<xuid>/settings.bin as records of { id, size } (host order) and the bytes.
    std::mutex g_settingsMutex;
    std::map<uint32_t, std::vector<uint8_t>> g_settings;
    bool g_settingsLoaded = false;

    fs::path SettingsPath()
    {
        return UserDir() / "profile" / ProfileFolder() / "settings.bin";
    }

    // Caller holds g_settingsMutex. Stops at the first record that is truncated or larger than its
    // setting allows (numeric values take 8 bytes), keeping the records before it.
    void LoadSettings()
    {
        if (g_settingsLoaded)
            return;
        g_settingsLoaded = true;
        std::ifstream file(SettingsPath(), std::ios::binary);
        uint32_t header[2];
        while (file.read(reinterpret_cast<char*>(header), sizeof(header)))
        {
            uint32_t id = header[0];
            uint32_t size = header[1];
            std::vector<uint8_t> value;
            if (size <= (HasPayload(id) ? SettingMaxSize(id) : 8u))
                value.resize(size);
            if (value.size() != size || !file.read(reinterpret_cast<char*>(value.data()), size))
            {
                Log("warning: %s is damaged, later profile settings are ignored", SettingsPath().string().c_str());
                break;
            }
            g_settings[id] = std::move(value);
        }
    }

    // Caller holds g_settingsMutex.
    bool SaveSettings()
    {
        fs::path path = SettingsPath();
        std::error_code error;
        fs::create_directories(path.parent_path(), error);
        fs::path temporary = path;
        temporary += ".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            for (auto& [id, value] : g_settings)
            {
                uint32_t header[2] = { id, uint32_t(value.size()) };
                file.write(reinterpret_cast<const char*>(header), sizeof(header));
                file.write(reinterpret_cast<const char*>(value.data()), value.size());
            }
            if (!file)
                return false;
        }
        fs::rename(temporary, path, error);
        return !error;
    }
}

// ---- Sign-in ----

// XamUserGetSigninState(user)
GUEST_FUNC(XamUserGetSigninState)
{
    ctx.r3.u64 = IsSignedIn(ctx.r3.u32) ? kSigninStateLocal : 0;
}

// XamUserGetXUID(user, xuidTypes, xuid*): the profile has only its offline XUID, which answers every
// type the game asks for (3 and 7).
GUEST_FUNC(XamUserGetXUID)
{
    if (ctx.r5.u32 == 0)
    {
        ctx.r3.u64 = kHresultInvalidArgument;
        return;
    }
    if (!IsSignedIn(ctx.r3.u32))
    {
        ctx.r3.u64 = kHresultNoSuchUser;
        return;
    }
    mem::Store64(ctx.r5.u32, kProfileXuid);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// XamUserGetSigninInfo(user, flags, XUSER_SIGNIN_INFO*): xuid, info flags, state, guest number,
// sponsor, name[16].
GUEST_FUNC(XamUserGetSigninInfo)
{
    uint32_t user = ctx.r3.u32;
    uint32_t flags = ctx.r4.u32;
    uint32_t info = ctx.r5.u32;
    if (info == 0)
    {
        ctx.r3.u64 = kHresultInvalidArgument;
        return;
    }
    memset(mem::Ptr(info), 0, 24 + kUserNameSize);
    if (!IsSignedIn(user))
    {
        ctx.r3.u64 = kHresultNoSuchUser;
        return;
    }
    // A local profile has no online XUID.
    mem::Store64(info + 0, (flags & kSigninOnlineXuidOnly) ? 0 : kProfileXuid);
    mem::Store32(info + 12, kSigninStateLocal);
    strncpy(reinterpret_cast<char*>(mem::Ptr(info + 24)), kProfileName, kUserNameSize - 1);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// XamUserGetName(user, buffer, size)
GUEST_FUNC(XamUserGetName)
{
    uint32_t user = ctx.r3.u32;
    uint32_t buffer = ctx.r4.u32;
    uint32_t size = ctx.r5.u32;
    if (!IsSignedIn(user))
    {
        ctx.r3.u64 = X_ERROR_NO_SUCH_USER;
        return;
    }
    if (size == 0)
    {
        ctx.r3.u64 = X_ERROR_INSUFFICIENT_BUFFER;
        return;
    }
    size_t length = std::min<size_t>(strlen(kProfileName), size - 1);
    memcpy(mem::Ptr(buffer), kProfileName, length);
    mem::Store8(buffer + uint32_t(length), 0);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// XamUserCheckPrivilege(user, privilege, result*): privileges belong to Live accounts, so a local
// profile has none. ERROR_NOT_LOGGED_ON is what the XDK wrapper itself returns for a local user
// on older system versions.
GUEST_FUNC(XamUserCheckPrivilege)
{
    uint32_t user = ctx.r3.u32;
    if (ctx.r5.u32 != 0)
        mem::Store32(ctx.r5.u32, 0);
    bool anyUser = user == 0xFF; // XUSER_INDEX_ANY
    ctx.r3.u64 = (anyUser || IsSignedIn(user)) ? X_ERROR_NOT_LOGGED_ON : X_ERROR_NO_SUCH_USER;
}

// XamShowSigninUI(panes, flags): the profile is already signed in and cannot change, so the UI
// only opens and closes.
GUEST_FUNC(XamShowSigninUI)
{
    PostNotification(kXnSysUi, 1);
    PostNotification(kXnSysUi, 0);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// ---- Profile settings ----

// XamUserReadProfileSettings(titleId, user, xuidCount, xuids, settingCount, settingIds, size*, buffer,
// overlapped). Reads the settings of the signed-in user (the game never passes XUIDs). With a null or
// short buffer, stores the size needed and fails with ERROR_INSUFFICIENT_BUFFER; the size reserves
// the maximum payload of every unicode and binary setting.
GUEST_FUNC(XamUserReadProfileSettings)
{
    uint32_t user = ctx.r4.u32;
    uint32_t xuidCount = ctx.r5.u32;
    uint32_t count = ctx.r7.u32;
    uint32_t ids = ctx.r8.u32;
    uint32_t sizeAddress = ctx.r9.u32;
    uint32_t buffer = ctx.r10.u32;
    uint32_t overlapped = StackArg32(ctx, 8);

    if (xuidCount != 0)
    {
        LOG_ONCE("unimplemented: XamUserReadProfileSettings for %u XUIDs", xuidCount);
        ctx.r3.u64 = CompleteOverlapped(overlapped, X_ERROR_INVALID_PARAMETER);
        return;
    }
    if (!IsSignedIn(user))
    {
        ctx.r3.u64 = CompleteOverlapped(overlapped, X_ERROR_NO_SUCH_USER);
        return;
    }

    uint32_t needed = kResultHeaderSize + count * kSettingSize;
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t id = mem::Load32(ids + i * 4);
        if (HasPayload(id))
            needed += SettingMaxSize(id);
    }
    if (buffer == 0 || mem::Load32(sizeAddress) < needed)
    {
        mem::Store32(sizeAddress, needed);
        ctx.r3.u64 = CompleteOverlapped(overlapped, X_ERROR_INSUFFICIENT_BUFFER);
        return;
    }

    memset(mem::Ptr(buffer), 0, needed);
    uint32_t settings = buffer + kResultHeaderSize;
    uint32_t payload = settings + count * kSettingSize;
    mem::Store32(buffer + 0, count);
    mem::Store32(buffer + 4, settings);

    std::lock_guard lock(g_settingsMutex);
    LoadSettings();
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t id = mem::Load32(ids + i * 4);
        uint32_t setting = settings + i * kSettingSize;
        mem::Store32(setting + 8, user);
        mem::Store32(setting + 16, id);
        mem::Store8(setting + 24, SettingType(id));

        uint32_t defaultValue = 0;
        auto stored = g_settings.find(id);
        if (stored != g_settings.end())
        {
            const auto& value = stored->second;
            mem::Store32(setting + 0, kSourceTitle);
            if (HasPayload(id))
            {
                mem::Store32(setting + 32, uint32_t(value.size()));
                mem::Store32(setting + 36, value.empty() ? 0 : payload);
                memcpy(mem::Ptr(payload), value.data(), value.size());
                payload += uint32_t(value.size());
            }
            else
            {
                memcpy(mem::Ptr(setting + 32), value.data(), std::min<size_t>(value.size(), 8));
            }
        }
        else if (DefaultSetting(id, defaultValue))
        {
            mem::Store32(setting + 0, kSourceDefault);
            mem::Store32(setting + 32, defaultValue);
        }
        else
        {
            mem::Store32(setting + 0, kSourceNoValue);
        }
    }
    ctx.r3.u64 = CompleteOverlapped(overlapped, X_ERROR_SUCCESS);
}

// XamUserWriteProfileSettings(titleId, user, settingCount, settings, overlapped): keeps the values
// for later reads and in settings.bin.
GUEST_FUNC(XamUserWriteProfileSettings)
{
    uint32_t user = ctx.r4.u32;
    uint32_t count = ctx.r5.u32;
    uint32_t settings = ctx.r6.u32;
    uint32_t overlapped = ctx.r7.u32;
    if (!IsSignedIn(user))
    {
        ctx.r3.u64 = CompleteOverlapped(overlapped, X_ERROR_NO_SUCH_USER);
        return;
    }

    std::lock_guard lock(g_settingsMutex);
    LoadSettings();
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t setting = settings + i * kSettingSize;
        uint32_t id = mem::Load32(setting + 16);
        std::vector<uint8_t> value;
        if (HasPayload(id))
        {
            uint32_t size = mem::Load32(setting + 32);
            uint32_t data = mem::Load32(setting + 36);
            if (size > SettingMaxSize(id) || (size != 0 && data == 0))
            {
                ctx.r3.u64 = CompleteOverlapped(overlapped, X_ERROR_INVALID_PARAMETER);
                return;
            }
            value.assign(mem::Ptr(data), mem::Ptr(data) + size);
        }
        else
        {
            value.assign(mem::Ptr(setting + 32), mem::Ptr(setting + 40));
        }
        g_settings[id] = std::move(value);
    }
    if (!SaveSettings())
        Log("warning: profile settings could not be written to %s", SettingsPath().string().c_str());
    ctx.r3.u64 = CompleteOverlapped(overlapped, X_ERROR_SUCCESS);
}
