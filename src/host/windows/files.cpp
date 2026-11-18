// Host file access on Windows: C streams, whole-file reads, positional files, file times (files.h).

#include "host/files.h"

#include <cstring>
#include <string>
#include <utility>
#include <windows.h>

namespace host
{
    namespace
    {
        HANDLE ToHandle(intptr_t handle)
        {
            return reinterpret_cast<HANDLE>(handle);
        }

        OVERLAPPED At(uint64_t offset)
        {
            OVERLAPPED overlapped{};
            overlapped.Offset = DWORD(offset);
            overlapped.OffsetHigh = DWORD(offset >> 32);
            return overlapped;
        }
    }

    std::FILE* OpenFile(const std::filesystem::path& path, const char* mode)
    {
        std::wstring wideMode(mode, mode + std::strlen(mode));
        return _wfopen(path.c_str(), wideMode.c_str());
    }

    std::vector<uint8_t> ReadWholeFile(const std::filesystem::path& path)
    {
        std::vector<uint8_t> data;
        std::FILE* file = OpenFile(path, "rb");
        if (file == nullptr)
            return data;
        if (_fseeki64(file, 0, SEEK_END) == 0)
        {
            long long size = _ftelli64(file);
            if (size > 0 && _fseeki64(file, 0, SEEK_SET) == 0)
            {
                data.resize(size_t(size));
                if (std::fread(data.data(), 1, data.size(), file) != data.size())
                    data.clear();
            }
        }
        std::fclose(file);
        return data;
    }

    File::~File()
    {
        if (IsOpen())
            CloseHandle(ToHandle(handle_));
    }

    File::File(File&& other) noexcept : handle_(std::exchange(other.handle_, -1)) {}

    File& File::operator=(File&& other) noexcept
    {
        if (this != &other)
        {
            if (IsOpen())
                CloseHandle(ToHandle(handle_));
            handle_ = std::exchange(other.handle_, -1);
        }
        return *this;
    }

    File File::Open(const std::filesystem::path& path, bool write, Disposition disposition, OpenError& error)
    {
        DWORD creation = CREATE_ALWAYS;
        switch (disposition)
        {
        case Disposition::OpenExisting: creation = OPEN_EXISTING; break;
        case Disposition::CreateNew: creation = CREATE_NEW; break;
        case Disposition::OpenAlways: creation = OPEN_ALWAYS; break;
        case Disposition::TruncateExisting: creation = TRUNCATE_EXISTING; break;
        case Disposition::CreateAlways: creation = CREATE_ALWAYS; break;
        }
        HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | (write ? GENERIC_WRITE : 0),
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            DWORD lastError = GetLastError();
            error = lastError == ERROR_PATH_NOT_FOUND ? OpenError::PathNotFound
                : lastError == ERROR_FILE_EXISTS      ? OpenError::Exists
                                                      : OpenError::Other;
            return File();
        }
        error = OpenError::None;
        return File(reinterpret_cast<intptr_t>(handle));
    }

    uint32_t File::Read(uint64_t offset, void* data, uint32_t length, uint32_t& done)
    {
        OVERLAPPED overlapped = At(offset);
        DWORD count = 0;
        BOOL ok = ReadFile(ToHandle(handle_), data, length, &count, &overlapped);
        DWORD lastError = ok ? ERROR_SUCCESS : GetLastError();
        done = count;
        return lastError == ERROR_HANDLE_EOF ? ERROR_SUCCESS : lastError;
    }

    uint32_t File::Write(uint64_t offset, const void* data, uint32_t length, uint32_t& done)
    {
        OVERLAPPED overlapped = At(offset);
        DWORD count = 0;
        BOOL ok = WriteFile(ToHandle(handle_), data, length, &count, &overlapped);
        done = count;
        return ok ? ERROR_SUCCESS : GetLastError();
    }

    bool File::Resize(uint64_t size)
    {
        LARGE_INTEGER end;
        end.QuadPart = int64_t(size);
        return SetFilePointerEx(ToHandle(handle_), end, nullptr, FILE_BEGIN) && SetEndOfFile(ToHandle(handle_));
    }

    uint64_t FileTime(std::filesystem::file_time_type time)
    {
        // file_time_type on Windows counts 100 ns ticks since 1601, like FILETIME.
        return uint64_t(time.time_since_epoch().count());
    }
}
