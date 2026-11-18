// Content packages (saved games), the storage device that holds them and XAM enumerators.

#include "kernel/kernel.h"
#include "kernel/xam.h"

#include "host/paths.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <type_traits>
#include <vector>

namespace fs = std::filesystem;
using namespace kernel;

// One device, the hard drive (id 1). A saved game is a folder,
// <user>/content/<xuid>/<content type>/<file name>/, mounted under the root name the title gives
// while the package is open. Beside the folder, <file name>.header holds the display name (the
// 128 big-endian UTF-16 units of XCONTENT_DATA) and <file name>.thumbnail the thumbnail bytes.
namespace
{
    constexpr uint32_t kDeviceId = 1;
    constexpr uint32_t kDeviceAny = 0; // XCONTENTDEVICE_ANY
    constexpr uint32_t kDeviceTypeHdd = 1;
    constexpr uint32_t kSavedGame = 1; // XCONTENTTYPE_SAVEDGAME

    // XCONTENT_DATA: device id, content type, display name (WCHAR[128]) at +8, file name
    // (CHAR[42]) at +264, 308 bytes with padding.
    constexpr uint32_t kContentDataSize = 308;
    constexpr uint32_t kDisplayNameOffset = 8;
    constexpr uint32_t kDisplayNameSize = 256;
    constexpr uint32_t kFileNameOffset = 264;
    constexpr uint32_t kFileNameSize = 42;

    // XDEVICE_DATA: id, type, total bytes, free bytes, friendly name (WCHAR[27]) at +24.
    constexpr uint32_t kDeviceDataSize = 80;
    constexpr uint32_t kDeviceNameLength = 27;

    // Creation dispositions (XCONTENTFLAG_CREATENEW..., the values of CreateFile's) and results.
    constexpr uint32_t kDispositionMask = 0xF;
    constexpr uint32_t kCreateNew = 1;        // XCONTENTFLAG_CREATENEW
    constexpr uint32_t kCreateAlways = 2;     // XCONTENTFLAG_CREATEALWAYS
    constexpr uint32_t kOpenExisting = 3;     // XCONTENTFLAG_OPENEXISTING
    constexpr uint32_t kOpenAlways = 4;       // XCONTENTFLAG_OPENALWAYS
    constexpr uint32_t kTruncateExisting = 5; // XCONTENTFLAG_TRUNCATEEXISTING
    constexpr uint32_t kCreatedNew = 1;      // XCONTENT_CREATED_NEW
    constexpr uint32_t kOpenedExisting = 2;  // XCONTENT_OPENED_EXISTING

    // Package creation, deletion and the header files: one at a time.
    std::mutex g_contentMutex;

    fs::path TypeDir(uint32_t type)
    {
        char typeName[9];
        snprintf(typeName, sizeof(typeName), "%08X", type);
        return UserDir() / "content" / ProfileFolder() / typeName;
    }

    // Entries of a folder, without exceptions (a folder that cannot be read gives what was read).
    std::vector<fs::directory_entry> ListFolder(const fs::path& dir)
    {
        std::vector<fs::directory_entry> entries;
        std::error_code error;
        fs::directory_iterator it(dir, error);
        for (; !error && it != fs::directory_iterator(); it.increment(error))
            entries.push_back(*it);
        return entries;
    }

    // The name of a package folder: printable ASCII only, so it fits the char file name of
    // XCONTENT_DATA. Read from the OS's own encoding, so no conversion can throw. Empty for any
    // other name.
    std::string PackageName(const fs::path& path)
    {
        std::string name;
        // Held in a variable: a range-for does not keep the temporary of filename() alive while it
        // walks the string native() returns into it.
        fs::path fileName = path.filename();
        for (fs::path::value_type c : fileName.native())
        {
            // Unsigned, so the bytes of a multibyte character fall outside the range.
            auto unit = static_cast<std::make_unsigned_t<fs::path::value_type>>(c);
            if (unit < 0x20 || unit > 0x7E)
                return {};
            name += char(unit);
        }
        return name;
    }

    // A package named by an XCONTENT_DATA. Its folder is matched by FindEntry, as the console matches
    // content names (the game opens "autosave.ninjasave" that it created as "Autosave.ninjasave"),
    // and the header and thumbnail files are named after the folder found.
    struct Package
    {
        uint32_t type = 0;
        std::string fileName;
        fs::path dir; // the folder as named, until Find

        // Sets dir to the package's folder on the disk, or to the folder as named when there is none.
        // Reads the folder of the type, so call it under g_contentMutex, with the checks that use dir.
        void Find()
        {
            FindEntry(TypeDir(type), fileName, dir);
        }

        fs::path Header() const { return Beside(".header"); }
        fs::path Thumbnail() const { return Beside(".thumbnail"); }

    private:
        fs::path Beside(const char* extension) const
        {
            fs::path path = dir;
            return path += extension;
        }
    };

    // A file name is one folder name: no separators, no device colon, no dot-only names.
    bool IsValidFileName(const std::string& name)
    {
        if (name.empty() || name == "." || name == "..")
            return false;
        return name.find_first_of("\\/:*?\"<>|") == std::string::npos;
    }

    // Decodes the XCONTENT_DATA at data. Only saved games on our device are supported; anything
    // else fails with the error the import returns.
    uint32_t LoadPackage(uint32_t data, Package& package)
    {
        if (data == 0)
            return X_ERROR_INVALID_PARAMETER;
        uint32_t device = mem::Load32(data + 0);
        if (device != kDeviceId && device != kDeviceAny)
            return X_ERROR_DEVICE_NOT_CONNECTED;
        package.type = mem::Load32(data + 4);
        if (package.type != kSavedGame)
        {
            LOG_ONCE("unimplemented: content packages of type %X", package.type);
            return X_ERROR_FUNCTION_FAILED;
        }
        const char* name = reinterpret_cast<const char*>(mem::Ptr(data + kFileNameOffset));
        package.fileName.assign(name, strnlen(name, kFileNameSize));
        if (!IsValidFileName(package.fileName))
            return X_ERROR_INVALID_PARAMETER;
        package.dir = TypeDir(package.type) / package.fileName;
        return X_ERROR_SUCCESS;
    }

    bool WriteHostFile(const fs::path& path, const void* data, size_t size)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(static_cast<const char*>(data), std::streamsize(size));
        return bool(file);
    }

    // XCONTENT_DATA of an existing package, for the enumerator.
    std::array<uint8_t, kContentDataSize> DescribePackage(uint32_t type, const std::string& fileName)
    {
        std::array<uint8_t, kContentDataSize> data{};
        uint32_t device = __builtin_bswap32(kDeviceId);
        uint32_t typeValue = __builtin_bswap32(type);
        memcpy(data.data() + 0, &device, 4);
        memcpy(data.data() + 4, &typeValue, 4);
        Package package{ type, fileName, TypeDir(type) / fileName };
        std::ifstream header(package.Header(), std::ios::binary);
        header.read(reinterpret_cast<char*>(data.data() + kDisplayNameOffset), kDisplayNameSize);
        memcpy(data.data() + kFileNameOffset, fileName.data(), std::min<size_t>(fileName.size(), kFileNameSize - 1));
        return data;
    }

    // XamCreateEnumerator-style handle over a fixed list of items, read in order by XamEnumerate.
    struct ContentEnumerator : KernelObject
    {
        ContentEnumerator() : KernelObject(ObjectType::Other) {}

        std::mutex mutex;
        std::vector<std::array<uint8_t, kContentDataSize>> items;
        size_t next = 0;
        uint32_t perCall = 1;
    };

    uint32_t CreateContent(uint32_t user, const std::string& root, uint32_t data, uint32_t flags,
        uint32_t dispositionOut, uint32_t licenseMaskOut)
    {
        if (!IsSignedIn(user))
            return X_ERROR_NO_SUCH_USER;
        Package package;
        uint32_t error = LoadPackage(data, package);
        if (error != X_ERROR_SUCCESS)
            return error;
        if (root.empty())
            return X_ERROR_INVALID_PARAMETER;

        std::lock_guard lock(g_contentMutex);
        if (IsDeviceMounted(root))
        {
            Log("content: root \"%s\" is already open", root.c_str());
            return X_ERROR_ALREADY_EXISTS;
        }

        package.Find();
        std::error_code fsError;
        bool exists = fs::is_directory(package.dir, fsError);
        uint32_t disposition = kOpenedExisting;
        bool clear = false;
        switch (flags & kDispositionMask)
        {
        case kCreateNew:
            if (exists)
                return X_ERROR_ALREADY_EXISTS;
            disposition = kCreatedNew;
            break;
        case kCreateAlways:
            // An existing package is replaced by an empty one.
            clear = exists;
            disposition = kCreatedNew;
            break;
        case kOpenExisting:
            if (!exists)
                return X_ERROR_FILE_NOT_FOUND;
            break;
        case kOpenAlways:
            disposition = exists ? kOpenedExisting : kCreatedNew;
            break;
        case kTruncateExisting:
            if (!exists)
                return X_ERROR_FILE_NOT_FOUND;
            clear = true;
            break;
        default:
            return X_ERROR_INVALID_PARAMETER;
        }

        if (clear)
        {
            for (auto& entry : ListFolder(package.dir))
                fs::remove_all(entry.path(), fsError);
        }
        if (disposition == kCreatedNew)
        {
            fs::create_directories(package.dir, fsError);
            if (!fs::is_directory(package.dir, fsError))
            {
                Log("content: cannot create %s", package.dir.string().c_str());
                return X_ERROR_ACCESS_DENIED;
            }
            if (!WriteHostFile(package.Header(), mem::Ptr(data + kDisplayNameOffset), kDisplayNameSize))
                Log("warning: content: cannot write %s", package.Header().string().c_str());
        }

        MountDevice(root, package.dir);
        Log("content: \"%s\" %s as %s:", package.fileName.c_str(),
            disposition == kCreatedNew ? "created" : "opened", root.c_str());
        if (dispositionOut != 0)
            mem::Store32(dispositionOut, disposition);
        if (licenseMaskOut != 0)
            mem::Store32(licenseMaskOut, 0); // saved games carry no license
        return X_ERROR_SUCCESS;
    }
}

// ---- Device ----

// XamContentGetDeviceState(device, overlapped)
GUEST_FUNC(XamContentGetDeviceState)
{
    uint32_t result = ctx.r3.u32 == kDeviceId ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
    ctx.r3.u64 = CompleteOverlapped(ctx.r4.u32, result);
}

// XamContentGetDeviceData(device, XDEVICE_DATA*): sizes come from the host disk of the user folder.
GUEST_FUNC(XamContentGetDeviceData)
{
    uint32_t device = ctx.r3.u32;
    uint32_t out = ctx.r4.u32;
    if (device != kDeviceId)
    {
        ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED;
        return;
    }
    std::error_code error;
    fs::create_directories(UserDir(), error);
    // The space this process may use, as the Xbox reports it for the user; zero when unknown.
    fs::space_info space = fs::space(UserDir(), error);
    if (error)
    {
        LOG_ONCE("warning: free space of %s unknown (%s)", host::PathText(UserDir()).c_str(), error.message().c_str());
        space = {};
    }

    memset(mem::Ptr(out), 0, kDeviceDataSize);
    mem::Store32(out + 0, kDeviceId);
    mem::Store32(out + 4, kDeviceTypeHdd);
    mem::Store64(out + 8, space.capacity);
    mem::Store64(out + 16, space.available);
    const char* name = "Hard Drive";
    for (uint32_t i = 0; name[i] != 0 && i < kDeviceNameLength - 1; i++)
        mem::Store16(out + 24 + i * 2, uint16_t(name[i]));
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// XamShowDeviceSelectorUI(user, contentType, flags, bytesRequested, deviceId*, overlapped): there is
// one device, so the selector picks it without showing anything.
GUEST_FUNC(XamShowDeviceSelectorUI)
{
    uint32_t deviceOut = ctx.r7.u32;
    PostNotification(kXnSysUi, 1);
    if (deviceOut != 0)
        mem::Store32(deviceOut, kDeviceId);
    ctx.r3.u64 = CompleteOverlapped(ctx.r8.u32, X_ERROR_SUCCESS);
    PostNotification(kXnSysUi, 0);
}

// ---- Packages ----

// XamContentCreateEx(user, rootName, XCONTENT_DATA*, flags, disposition*, licenseMask*, cacheSize,
// contentSize, overlapped)
GUEST_FUNC(XamContentCreateEx)
{
    std::string root = ctx.r4.u32 != 0 ? mem::String(ctx.r4.u32) : "";
    uint32_t result = CreateContent(ctx.r3.u32, root, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
    ctx.r3.u64 = CompleteOverlapped(StackArg32(ctx, 8), result);
}

// XamContentClose(rootName, overlapped): files opened through the root stay usable.
GUEST_FUNC(XamContentClose)
{
    std::string root = ctx.r3.u32 != 0 ? mem::String(ctx.r3.u32) : "";
    uint32_t result = UnmountDevice(root) ? X_ERROR_SUCCESS : X_ERROR_NOT_FOUND;
    ctx.r3.u64 = CompleteOverlapped(ctx.r4.u32, result);
}

// XamContentFlush(rootName, overlapped): writes already reach the host files.
GUEST_FUNC(XamContentFlush)
{
    std::string root = ctx.r3.u32 != 0 ? mem::String(ctx.r3.u32) : "";
    uint32_t result = IsDeviceMounted(root) ? X_ERROR_SUCCESS : X_ERROR_NOT_FOUND;
    ctx.r3.u64 = CompleteOverlapped(ctx.r4.u32, result);
}

// XamContentDelete(user, XCONTENT_DATA*, overlapped)
GUEST_FUNC(XamContentDelete)
{
    uint32_t overlapped = ctx.r5.u32;
    Package package;
    uint32_t result = IsSignedIn(ctx.r3.u32) ? LoadPackage(ctx.r4.u32, package) : X_ERROR_NO_SUCH_USER;
    if (result == X_ERROR_SUCCESS)
    {
        std::lock_guard lock(g_contentMutex);
        package.Find();
        std::error_code error;
        if (!fs::is_directory(package.dir, error))
        {
            result = X_ERROR_FILE_NOT_FOUND;
        }
        else
        {
            fs::remove_all(package.dir, error);
            fs::remove(package.Header(), error);
            fs::remove(package.Thumbnail(), error);
            Log("content: \"%s\" deleted", package.fileName.c_str());
        }
    }
    ctx.r3.u64 = CompleteOverlapped(overlapped, result);
}

// XamContentGetCreator(user, XCONTENT_DATA*, isCreator*, xuid*, overlapped): every package was made
// by the one profile.
GUEST_FUNC(XamContentGetCreator)
{
    uint32_t isCreatorOut = ctx.r5.u32;
    uint32_t xuidOut = ctx.r6.u32;
    uint32_t overlapped = ctx.r7.u32;
    Package package;
    uint32_t result = IsSignedIn(ctx.r3.u32) ? LoadPackage(ctx.r4.u32, package) : X_ERROR_NO_SUCH_USER;
    if (result == X_ERROR_SUCCESS)
    {
        std::lock_guard lock(g_contentMutex);
        package.Find();
        std::error_code error;
        if (!fs::is_directory(package.dir, error))
            result = X_ERROR_FILE_NOT_FOUND;
    }
    if (result == X_ERROR_SUCCESS)
    {
        if (isCreatorOut != 0)
            mem::Store32(isCreatorOut, 1);
        if (xuidOut != 0)
            mem::Store64(xuidOut, kProfileXuid);
    }
    ctx.r3.u64 = CompleteOverlapped(overlapped, result);
}

// XamContentSetThumbnail(user, XCONTENT_DATA*, thumbnail, size, overlapped)
GUEST_FUNC(XamContentSetThumbnail)
{
    uint32_t thumbnail = ctx.r5.u32;
    uint32_t size = ctx.r6.u32;
    uint32_t overlapped = ctx.r7.u32;
    Package package;
    uint32_t result = IsSignedIn(ctx.r3.u32) ? LoadPackage(ctx.r4.u32, package) : X_ERROR_NO_SUCH_USER;
    if (result == X_ERROR_SUCCESS && (thumbnail == 0 || size == 0))
        result = X_ERROR_INVALID_PARAMETER;
    if (result == X_ERROR_SUCCESS)
    {
        std::lock_guard lock(g_contentMutex);
        package.Find();
        std::error_code error;
        if (!fs::is_directory(package.dir, error))
        {
            result = X_ERROR_FILE_NOT_FOUND;
        }
        else
        {
            mem::Commit(thumbnail, size);
            if (WriteHostFile(package.Thumbnail(), mem::Ptr(thumbnail), size))
                Log("content: thumbnail of \"%s\" set (%u bytes)", package.fileName.c_str(), size);
            else
                result = X_ERROR_ACCESS_DENIED;
        }
    }
    ctx.r3.u64 = CompleteOverlapped(overlapped, result);
}

// ---- Enumeration ----

// XamContentCreateEnumerator(user, device, contentType, flags, itemsPerCall, bufferSize*, handle*):
// lists the saved games of the profile. Other content types (the game asks for marketplace content
// of users 0-3 at boot) have no packages.
GUEST_FUNC(XamContentCreateEnumerator)
{
    uint32_t user = ctx.r3.u32;
    uint32_t device = ctx.r4.u32;
    uint32_t type = ctx.r5.u32;
    uint32_t perCall = ctx.r7.u32;
    uint32_t sizeOut = ctx.r8.u32;
    uint32_t handleOut = ctx.r9.u32;

    if (device != kDeviceId && device != kDeviceAny)
    {
        ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED;
        return;
    }
    if (type == kSavedGame && !IsSignedIn(user))
    {
        ctx.r3.u64 = X_ERROR_NO_SUCH_USER;
        return;
    }
    if (perCall == 0 || handleOut == 0)
    {
        ctx.r3.u64 = X_ERROR_INVALID_PARAMETER;
        return;
    }

    auto enumerator = std::make_shared<ContentEnumerator>();
    enumerator->perCall = perCall;
    if (type == kSavedGame)
    {
        std::lock_guard lock(g_contentMutex);
        std::error_code error;
        for (auto& entry : ListFolder(TypeDir(type)))
        {
            std::string name = PackageName(entry.path());
            if (entry.is_directory(error) && name.size() < kFileNameSize && IsValidFileName(name))
                enumerator->items.push_back(DescribePackage(type, name));
        }
    }
    if (sizeOut != 0)
        mem::Store32(sizeOut, kContentDataSize * perCall);
    mem::Store32(handleOut, CreateHandle(enumerator));
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// XamEnumerate(handle, flags, buffer, size, itemsReturned*, overlapped): the next items, as many as
// the enumerator returns per call and the buffer holds; ERROR_NO_MORE_FILES at the end.
GUEST_FUNC(XamEnumerate)
{
    uint32_t buffer = ctx.r5.u32;
    uint32_t size = ctx.r6.u32;
    uint32_t countOut = ctx.r7.u32;
    uint32_t overlapped = ctx.r8.u32;
    if (countOut != 0)
        mem::Store32(countOut, 0);

    auto enumerator = std::dynamic_pointer_cast<ContentEnumerator>(ObjectFromHandle(ctx.r3.u32));
    if (enumerator == nullptr)
    {
        LOG_ONCE("XamEnumerate: handle %08X is not an enumerator", ctx.r3.u32);
        ctx.r3.u64 = CompleteOverlapped(overlapped, X_ERROR_INVALID_HANDLE);
        return;
    }

    std::lock_guard lock(enumerator->mutex);
    size_t remaining = enumerator->items.size() - enumerator->next;
    if (remaining == 0)
    {
        ctx.r3.u64 = CompleteOverlapped(overlapped, X_ERROR_NO_MORE_FILES);
        return;
    }
    if (size < kContentDataSize)
    {
        ctx.r3.u64 = CompleteOverlapped(overlapped, X_ERROR_INSUFFICIENT_BUFFER);
        return;
    }
    size_t count = std::min<size_t>({ enumerator->perCall, size / kContentDataSize, remaining });
    for (size_t i = 0; i < count; i++)
        memcpy(mem::Ptr(buffer + uint32_t(i) * kContentDataSize), enumerator->items[enumerator->next++].data(), kContentDataSize);
    if (countOut != 0)
        mem::Store32(countOut, uint32_t(count));
    ctx.r3.u64 = CompleteOverlapped(overlapped, X_ERROR_SUCCESS, uint32_t(count));
}
