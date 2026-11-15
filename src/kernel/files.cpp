// File system: guest paths are mapped to the game folder (read-only) or to a user data folder.

#include "kernel/kernel.h"

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <windows.h>

namespace fs = std::filesystem;
using namespace kernel;

namespace
{
    fs::path g_gameDir;
    fs::path g_userDir;

    std::mutex g_linkMutex;
    std::unordered_map<std::string, std::string> g_symbolicLinks; // lower-case link -> target

    std::mutex g_mountMutex;
    std::unordered_map<std::string, fs::path> g_mounts; // lower-case device -> host folder

    // FILE_ATTRIBUTE_* come from windows.h and have the same values on the Xbox 360.
    constexpr uint32_t kFileDirectoryFile = 0x01; // NtCreateFile option

    std::string Lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(tolower(c)); });
        return text;
    }

    struct FileObject : KernelObject
    {
        FileObject() : KernelObject(ObjectType::File) {}
        ~FileObject() override
        {
            if (handle != INVALID_HANDLE_VALUE)
                ::CloseHandle(handle);
        }

        std::mutex mutex;
        HANDLE handle = INVALID_HANDLE_VALUE;
        fs::path path;
        std::string guestPath;
        bool directory = false;
        bool readOnly = true;
        uint64_t position = 0;

        // Directory enumeration.
        std::vector<fs::directory_entry> entries;
        size_t nextEntry = 0;
        bool enumerated = false;
    };

    // Splits "game:\a\b", "\Device\Cdrom0\a\b" or "\??\game:\a\b" into device and relative path,
    // following the symbolic links the game created.
    bool ResolvePath(std::string guestPath, fs::path& hostPath, bool& readOnly)
    {
        std::replace(guestPath.begin(), guestPath.end(), '/', '\\');

        for (int depth = 0; depth < 4; depth++)
        {
            std::lock_guard lock(g_linkMutex);
            std::string lower = Lower(guestPath);
            bool replaced = false;
            for (auto& [link, target] : g_symbolicLinks)
            {
                if (lower.compare(0, link.size(), link) == 0)
                {
                    guestPath = target + guestPath.substr(link.size());
                    replaced = true;
                    break;
                }
            }
            if (!replaced)
                break;
        }

        std::string path = guestPath;
        if (path.rfind("\\??\\", 0) == 0)
            path = path.substr(4);

        std::string device;
        std::string rest;
        size_t colon = path.find(':');
        std::string lower = Lower(path);
        if (lower.rfind("\\device\\", 0) == 0)
        {
            size_t end = path.find('\\', 8);
            device = Lower(path.substr(8, end == std::string::npos ? std::string::npos : end - 8));
            rest = end == std::string::npos ? "" : path.substr(end);
            if (device == "harddisk0")
            {
                size_t partitionEnd = rest.find('\\', 1);
                device += Lower(rest.substr(0, partitionEnd));
                rest = partitionEnd == std::string::npos ? "" : rest.substr(partitionEnd);
            }
        }
        else if (colon != std::string::npos)
        {
            device = Lower(path.substr(0, colon));
            rest = path.substr(colon + 1);
        }
        else
        {
            device = "game";
            rest = path;
        }

        while (!rest.empty() && rest.front() == '\\')
            rest.erase(rest.begin());

        {
            std::lock_guard lock(g_mountMutex);
            auto mount = g_mounts.find(device);
            if (mount != g_mounts.end())
            {
                hostPath = mount->second / rest;
                readOnly = false;
                return true;
            }
        }

        if (device == "game" || device == "d" || device == "cdrom0" || device == "media")
        {
            hostPath = g_gameDir / rest;
            readOnly = true;
        }
        else
        {
            std::replace(device.begin(), device.end(), '\\', '_');
            hostPath = g_userDir / device / rest;
            readOnly = false;
        }
        return true;
    }

    uint64_t ToFileTime(fs::file_time_type time)
    {
        // file_time_type on Windows counts 100 ns ticks since 1601, like FILETIME.
        return uint64_t(time.time_since_epoch().count());
    }

    void StoreNetworkOpenInfo(uint32_t info, const fs::path& path)
    {
        std::error_code error;
        bool directory = fs::is_directory(path, error);
        uint64_t size = directory ? 0 : fs::file_size(path, error);
        uint64_t time = ToFileTime(fs::last_write_time(path, error));
        for (int i = 0; i < 4; i++)
            mem::Store64(info + i * 8, time);
        mem::Store64(info + 32, (size + 4095) & ~4095ull);
        mem::Store64(info + 40, size);
        mem::Store32(info + 48, directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL);
        mem::Store32(info + 52, 0);
    }

    void StoreIoStatus(uint32_t ioStatus, uint32_t status, uint32_t information)
    {
        if (ioStatus != 0)
        {
            mem::Store32(ioStatus, status);
            mem::Store32(ioStatus + 4, information);
        }
    }

    std::shared_ptr<FileObject> FileFromHandle(uint32_t handle)
    {
        return std::dynamic_pointer_cast<FileObject>(ObjectFromHandle(handle));
    }

    // NtCreateFile/NtOpenFile.
    uint32_t OpenFile(uint32_t handlePtr, uint32_t access, uint32_t attributes, uint32_t ioStatus,
        uint32_t disposition, uint32_t options)
    {
        std::string guestPath = LoadAnsiString(mem::Load32(attributes + 4));
        uint32_t root = mem::Load32(attributes + 0);
        if (root != 0)
        {
            auto rootFile = FileFromHandle(root);
            if (rootFile != nullptr)
                guestPath = rootFile->guestPath + "\\" + guestPath;
        }

        fs::path hostPath;
        bool readOnly = true;
        ResolvePath(guestPath, hostPath, readOnly);

        bool wantsWrite = (access & (0x40000000 | 0x2 | 0x4 | 0x10000)) != 0 || (disposition != 1 && disposition != 3);
        std::error_code error;
        bool exists = fs::exists(hostPath, error);
        bool isDirectory = exists && fs::is_directory(hostPath, error);

        if (readOnly && wantsWrite && !(exists && (disposition == 1 || disposition == 3)))
        {
            Log("file: write access to \"%s\" denied (game files are read-only)", guestPath.c_str());
            StoreIoStatus(ioStatus, X_STATUS_ACCESS_DENIED, 0);
            return X_STATUS_ACCESS_DENIED;
        }

        auto file = std::make_shared<FileObject>();
        file->path = hostPath;
        file->guestPath = guestPath;
        uint32_t information = 1; // FILE_OPENED

        if ((options & kFileDirectoryFile) || isDirectory)
        {
            if (!exists)
            {
                if (disposition == 1 || readOnly)
                {
                    TRACE("file: \"%s\" not found", guestPath.c_str());
                    StoreIoStatus(ioStatus, X_STATUS_OBJECT_PATH_NOT_FOUND, 0);
                    return X_STATUS_OBJECT_PATH_NOT_FOUND;
                }
                fs::create_directories(hostPath, error);
                information = 2; // FILE_CREATED
            }
            file->directory = true;
        }
        else
        {
            DWORD creation;
            switch (disposition)
            {
            case 1: creation = OPEN_EXISTING; break;
            case 2: creation = CREATE_NEW; break;
            case 3: creation = OPEN_ALWAYS; break;
            case 4: creation = TRUNCATE_EXISTING; break;
            default: creation = CREATE_ALWAYS; break;
            }
            bool write = !readOnly && wantsWrite;
            if (write)
                fs::create_directories(hostPath.parent_path(), error);
            file->handle = CreateFileW(hostPath.c_str(), GENERIC_READ | (write ? GENERIC_WRITE : 0),
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file->handle == INVALID_HANDLE_VALUE)
            {
                DWORD lastError = GetLastError();
                uint32_t status = lastError == ERROR_PATH_NOT_FOUND ? X_STATUS_OBJECT_PATH_NOT_FOUND
                    : lastError == ERROR_FILE_EXISTS ? X_STATUS_OBJECT_NAME_COLLISION
                    : X_STATUS_OBJECT_NAME_NOT_FOUND;
                Log("file: \"%s\" not found", guestPath.c_str());
                StoreIoStatus(ioStatus, status, 0);
                return status;
            }
            file->readOnly = !write;
            if (!exists)
                information = 2;
            else if (disposition == 4 || disposition == 5 || disposition == 0)
                information = 3; // FILE_OVERWRITTEN
        }

        Log("file: opened \"%s\"", guestPath.c_str());
        mem::Store32(handlePtr, CreateHandle(file));
        StoreIoStatus(ioStatus, X_STATUS_SUCCESS, information);
        return X_STATUS_SUCCESS;
    }

    bool WildcardMatch(const char* pattern, const char* text)
    {
        if (*pattern == 0)
            return *text == 0;
        if (*pattern == '*')
            return WildcardMatch(pattern + 1, text) || (*text != 0 && WildcardMatch(pattern, text + 1));
        if (*text == 0)
            return false;
        if (*pattern == '?' || tolower((unsigned char)*pattern) == tolower((unsigned char)*text))
            return WildcardMatch(pattern + 1, text + 1);
        return false;
    }
}

namespace kernel
{
    void SetPaths(const std::string& gameDir, const std::string& userDir)
    {
        g_gameDir = gameDir;
        g_userDir = userDir;
    }

    std::filesystem::path HostPath(const std::string& guestPath)
    {
        fs::path hostPath;
        bool readOnly;
        ResolvePath(guestPath, hostPath, readOnly);
        return hostPath;
    }

    const std::filesystem::path& UserDir()
    {
        return g_userDir;
    }

    bool MountDevice(const std::string& name, const std::filesystem::path& hostDir)
    {
        std::lock_guard lock(g_mountMutex);
        return g_mounts.emplace(Lower(name), hostDir).second;
    }

    bool UnmountDevice(const std::string& name)
    {
        std::lock_guard lock(g_mountMutex);
        return g_mounts.erase(Lower(name)) != 0;
    }

    bool IsDeviceMounted(const std::string& name)
    {
        std::lock_guard lock(g_mountMutex);
        return g_mounts.contains(Lower(name));
    }
}

// NtCreateFile(handle*, access, attributes, ioStatus, allocationSize*, fileAttributes, share,
//              disposition, options)
GUEST_FUNC(NtCreateFile)
{
    ctx.r3.u64 = OpenFile(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r10.u32, StackArg32(ctx, 8));
}

// NtOpenFile(handle*, access, attributes, ioStatus, share, options)
GUEST_FUNC(NtOpenFile)
{
    ctx.r3.u64 = OpenFile(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 1, ctx.r8.u32);
}

namespace
{
    // Shared by NtReadFile and NtWriteFile: (handle, event, apcRoutine, apcContext, ioStatus,
    // buffer, length, byteOffset*).
    void TransferFile(PPCContext& ctx, bool write)
    {
        auto file = FileFromHandle(ctx.r3.u32);
        if (file == nullptr || file->directory)
        {
            ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
            return;
        }
        uint32_t ioStatus = ctx.r7.u32;
        uint32_t buffer = ctx.r8.u32;
        uint32_t length = ctx.r9.u32;
        uint32_t offsetPtr = ctx.r10.u32;

        std::lock_guard lock(file->mutex);
        uint64_t offset = offsetPtr != 0 ? mem::Load64(offsetPtr) : file->position;
        if (offset == 0xFFFFFFFFFFFFFFFEull) // FILE_USE_FILE_POINTER_POSITION
            offset = file->position;

        OVERLAPPED overlapped{};
        overlapped.Offset = DWORD(offset);
        overlapped.OffsetHigh = DWORD(offset >> 32);
        DWORD done = 0;
        mem::Commit(buffer, length);
        BOOL ok = write ? WriteFile(file->handle, mem::Ptr(buffer), length, &done, &overlapped)
                        : ReadFile(file->handle, mem::Ptr(buffer), length, &done, &overlapped);
        if (!ok && GetLastError() != ERROR_HANDLE_EOF)
            Log("warning: %s of \"%s\" failed (%lu)", write ? "write" : "read", file->guestPath.c_str(), GetLastError());
        file->position = offset + done;

        uint32_t status = (!write && done == 0 && length != 0) ? X_STATUS_END_OF_FILE : X_STATUS_SUCCESS;
        TRACE("file: %s \"%s\" offset %llX length %X -> %lX (status %08X, event %08X)", write ? "write" : "read",
            file->guestPath.c_str(), (unsigned long long)offset, length, done, status, ctx.r4.u32);
        StoreIoStatus(ioStatus, status, done);
        if (ctx.r4.u32 != 0)
        {
            auto event = ObjectFromHandle(ctx.r4.u32);
            if (event != nullptr)
                SetEvent(event);
        }
        if (ctx.r5.u32 != 0)
            Log("warning: file APC routine %08X is not delivered", ctx.r5.u32);
        ctx.r3.u64 = status;
    }
}

GUEST_FUNC(NtReadFile) { TransferFile(ctx, false); }
GUEST_FUNC(NtWriteFile) { TransferFile(ctx, true); }

// NtReadFileScatter(handle, event, apcRoutine, apcContext, ioStatus, segments, length, byteOffset*)
// Segments are guest addresses of 4 KB pages.
GUEST_FUNC(NtReadFileScatter)
{
    auto file = FileFromHandle(ctx.r3.u32);
    if (file == nullptr)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }
    uint32_t segments = ctx.r8.u32;
    uint32_t length = ctx.r9.u32;

    std::lock_guard lock(file->mutex);
    uint64_t offset = ctx.r10.u32 != 0 ? mem::Load64(ctx.r10.u32) : file->position;
    uint32_t total = 0;
    for (uint32_t i = 0; total < length; i++)
    {
        uint32_t page = mem::Load32(segments + i * 4);
        uint32_t chunk = std::min<uint32_t>(4096, length - total);
        OVERLAPPED overlapped{};
        overlapped.Offset = DWORD(offset + total);
        overlapped.OffsetHigh = DWORD((offset + total) >> 32);
        DWORD done = 0;
        mem::Commit(page, chunk);
        ReadFile(file->handle, mem::Ptr(page), chunk, &done, &overlapped);
        total += done;
        if (done < chunk)
            break;
    }
    file->position = offset + total;

    uint32_t status = (total == 0 && length != 0) ? X_STATUS_END_OF_FILE : X_STATUS_SUCCESS;
    StoreIoStatus(ctx.r7.u32, status, total);
    if (ctx.r4.u32 != 0)
    {
        auto event = ObjectFromHandle(ctx.r4.u32);
        if (event != nullptr)
            SetEvent(event);
    }
    ctx.r3.u64 = status;
}

// NtQueryInformationFile(handle, ioStatus, info, length, infoClass)
GUEST_FUNC(NtQueryInformationFile)
{
    auto file = FileFromHandle(ctx.r3.u32);
    if (file == nullptr)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }
    uint32_t info = ctx.r5.u32;
    uint32_t infoClass = ctx.r7.u32;
    std::error_code error;
    uint64_t size = file->directory ? 0 : fs::file_size(file->path, error);
    uint32_t written = 0;

    switch (infoClass)
    {
    case 4: // FileBasicInformation
    {
        uint64_t time = ToFileTime(fs::last_write_time(file->path, error));
        for (int i = 0; i < 4; i++)
            mem::Store64(info + i * 8, time);
        mem::Store32(info + 32, file->directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL);
        written = 40;
        break;
    }
    case 5: // FileStandardInformation
        mem::Store64(info + 0, (size + 4095) & ~4095ull);
        mem::Store64(info + 8, size);
        mem::Store32(info + 16, 1);
        mem::Store8(info + 20, 0);
        mem::Store8(info + 21, file->directory ? 1 : 0);
        written = 24;
        break;
    case 6: // FileInternalInformation
        mem::Store64(info, std::hash<std::string>{}(file->guestPath));
        written = 8;
        break;
    case 14: // FilePositionInformation
        mem::Store64(info, file->position);
        written = 8;
        break;
    case 17: // FileAlignmentInformation
        mem::Store32(info, 0);
        written = 4;
        break;
    case 34: // FileNetworkOpenInformation
        StoreNetworkOpenInfo(info, file->path);
        written = 56;
        break;
    default:
        Log("warning: NtQueryInformationFile class %u not supported", infoClass);
        ctx.r3.u64 = X_STATUS_INVALID_INFO_CLASS;
        return;
    }
    StoreIoStatus(ctx.r4.u32, X_STATUS_SUCCESS, written);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// NtSetInformationFile(handle, ioStatus, info, length, infoClass)
GUEST_FUNC(NtSetInformationFile)
{
    auto file = FileFromHandle(ctx.r3.u32);
    if (file == nullptr)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }
    uint32_t info = ctx.r5.u32;
    switch (ctx.r7.u32)
    {
    case 14: // FilePositionInformation
        file->position = mem::Load64(info);
        break;
    case 20: // FileEndOfFileInformation
        if (!file->readOnly)
        {
            LARGE_INTEGER end;
            end.QuadPart = int64_t(mem::Load64(info));
            SetFilePointerEx(file->handle, end, nullptr, FILE_BEGIN);
            SetEndOfFile(file->handle);
        }
        break;
    default:
        TRACE("NtSetInformationFile class %u ignored", ctx.r7.u32);
        break;
    }
    StoreIoStatus(ctx.r4.u32, X_STATUS_SUCCESS, 0);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// NtQueryFullAttributesFile(attributes, FILE_NETWORK_OPEN_INFORMATION*)
GUEST_FUNC(NtQueryFullAttributesFile)
{
    std::string guestPath = LoadAnsiString(mem::Load32(ctx.r3.u32 + 4));
    fs::path hostPath;
    bool readOnly;
    ResolvePath(guestPath, hostPath, readOnly);
    std::error_code error;
    if (!fs::exists(hostPath, error))
    {
        TRACE("file: attributes of \"%s\": not found", guestPath.c_str());
        ctx.r3.u64 = X_STATUS_OBJECT_NAME_NOT_FOUND;
        return;
    }
    StoreNetworkOpenInfo(ctx.r4.u32, hostPath);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// NtQueryVolumeInformationFile(handle, ioStatus, info, length, infoClass)
GUEST_FUNC(NtQueryVolumeInformationFile)
{
    uint32_t info = ctx.r5.u32;
    uint32_t written = 0;
    switch (ctx.r7.u32)
    {
    case 1: // FileFsVolumeInformation
        mem::Store64(info + 0, SystemTime());
        mem::Store32(info + 8, 0x12345678);
        mem::Store32(info + 12, 0);
        mem::Store8(info + 16, 0);
        written = 18;
        break;
    case 3: // FileFsSizeInformation
        mem::Store64(info + 0, 0x100000);
        mem::Store64(info + 8, 0x80000);
        mem::Store32(info + 16, 8);
        mem::Store32(info + 20, 512);
        written = 24;
        break;
    case 5: // FileFsAttributeInformation
        mem::Store32(info + 0, 0);
        mem::Store32(info + 4, 255);
        mem::Store32(info + 8, 4);
        memcpy(mem::Ptr(info + 12), "FATX", 4);
        written = 16;
        break;
    default:
        Log("warning: NtQueryVolumeInformationFile class %u not supported", ctx.r7.u32);
        ctx.r3.u64 = X_STATUS_INVALID_INFO_CLASS;
        return;
    }
    StoreIoStatus(ctx.r4.u32, X_STATUS_SUCCESS, written);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// NtQueryDirectoryFile(handle, event, apcRoutine, apcContext, ioStatus, info, length, pattern,
//                      restartScan). Returns one FILE_DIRECTORY_INFORMATION per call.
GUEST_FUNC(NtQueryDirectoryFile)
{
    auto file = FileFromHandle(ctx.r3.u32);
    if (file == nullptr || !file->directory)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }
    uint32_t ioStatus = ctx.r7.u32;
    uint32_t info = ctx.r8.u32;
    uint32_t length = ctx.r9.u32;
    std::string pattern = ctx.r10.u32 != 0 ? LoadAnsiString(ctx.r10.u32) : "";
    // No name or an empty one lists everything (FindFirstFileA("D:\*.*") passes an empty name), and
    // "*.*" also matches names without a dot, as on DOS.
    if (pattern.empty() || pattern == "*.*")
        pattern = "*";
    bool restart = StackArg32(ctx, 8) != 0;

    std::lock_guard lock(file->mutex);
    if (!file->enumerated || restart)
    {
        file->entries.clear();
        std::error_code error;
        for (auto& entry : fs::directory_iterator(file->path, error))
        {
            if (WildcardMatch(pattern.c_str(), entry.path().filename().string().c_str()))
                file->entries.push_back(entry);
        }
        file->nextEntry = 0;
        file->enumerated = true;
        TRACE("file: listing \"%s\" with \"%s\": %zu entries", file->guestPath.c_str(), pattern.c_str(), file->entries.size());
    }

    if (file->nextEntry >= file->entries.size())
    {
        StoreIoStatus(ioStatus, X_STATUS_NO_MORE_FILES, 0);
        ctx.r3.u64 = X_STATUS_NO_MORE_FILES;
        return;
    }

    const auto& entry = file->entries[file->nextEntry];
    std::string name = entry.path().filename().string();
    uint32_t needed = 64 + uint32_t(name.size());
    if (length < needed)
    {
        ctx.r3.u64 = X_STATUS_BUFFER_TOO_SMALL;
        return;
    }
    file->nextEntry++;

    std::error_code error;
    bool directory = entry.is_directory(error);
    uint64_t size = directory ? 0 : entry.file_size(error);
    uint64_t time = ToFileTime(entry.last_write_time(error));
    mem::Store32(info + 0, 0);
    mem::Store32(info + 4, uint32_t(file->nextEntry));
    for (int i = 0; i < 4; i++)
        mem::Store64(info + 8 + i * 8, time);
    mem::Store64(info + 40, size);
    mem::Store64(info + 48, (size + 4095) & ~4095ull);
    mem::Store32(info + 56, directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL);
    mem::Store32(info + 60, uint32_t(name.size()));
    memcpy(mem::Ptr(info + 64), name.data(), name.size());

    StoreIoStatus(ioStatus, X_STATUS_SUCCESS, needed);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

GUEST_FUNC(NtFlushBuffersFile) { ctx.r3.u64 = X_STATUS_SUCCESS; }
GUEST_FUNC(IoDismountVolume) { ctx.r3.u64 = X_STATUS_SUCCESS; }
GUEST_FUNC(IoDismountVolumeByFileHandle) { ctx.r3.u64 = X_STATUS_SUCCESS; }

// ObCreateSymbolicLink(link, target): both ANSI_STRING*
GUEST_FUNC(ObCreateSymbolicLink)
{
    std::string link = LoadAnsiString(ctx.r3.u32);
    std::string target = LoadAnsiString(ctx.r4.u32);
    Log("file: symbolic link \"%s\" -> \"%s\"", link.c_str(), target.c_str());
    std::lock_guard lock(g_linkMutex);
    g_symbolicLinks[Lower(link)] = target;
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

GUEST_FUNC(ObDeleteSymbolicLink)
{
    std::lock_guard lock(g_linkMutex);
    g_symbolicLinks.erase(Lower(LoadAnsiString(ctx.r3.u32)));
    ctx.r3.u64 = X_STATUS_SUCCESS;
}
