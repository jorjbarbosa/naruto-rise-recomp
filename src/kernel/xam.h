#pragma once

// Shared by the XAM files (xam.cpp, profile.cpp, content.cpp): overlapped completion,
// notifications and the signed-in profile.

#include <cstdint>
#include <cstdio>
#include <string>

namespace kernel
{
    // Completes an XOVERLAPPED at once with result (a Win32 error code) and length, and sets its
    // event. Returns what an XAM import returns: result itself when there is no overlapped,
    // X_ERROR_IO_PENDING otherwise.
    uint32_t CompleteOverlapped(uint32_t overlapped, uint32_t result, uint32_t length = 0);

    // Notification ids, XNID(version, area, index) = version << 25 | area << 16 | index.
    constexpr uint32_t kXnSysUi = 0x00000009; // parameter: 1 when a system UI opens, 0 when it closes

    // Queues a notification for every listener whose area mask covers its area. Thread-safe.
    void PostNotification(uint32_t id, uint32_t param);

    // The only profile: signed in locally on user 0 for the whole run (no online account). The
    // offline XUID and the name never change.
    constexpr uint32_t kProfileUser = 0;
    constexpr uint64_t kProfileXuid = 0xE000000000000001ull;
    constexpr const char* kProfileName = "Player";

    inline bool IsSignedIn(uint32_t user) { return user == kProfileUser; }

    // The folder name of the profile under <user>/profile and <user>/content: the XUID in hex.
    inline std::string ProfileFolder()
    {
        char name[17];
        snprintf(name, sizeof(name), "%016llX", (unsigned long long)kProfileXuid);
        return name;
    }
}
