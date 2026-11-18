// Host file access on POSIX: C streams, whole-file reads, positional files, file times (files.h).

#include "host/files.h"

#include <cerrno>
#include <chrono>
#include <climits>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace host
{
    namespace
    {
        // From 1601-01-01 to 1970-01-01, in 100 ns intervals.
        constexpr uint64_t kUnixEpochIn1601 = 116'444'736'000'000'000ull;

        // Why open failed with error, in File::Open's terms. POSIX gives ENOENT for a missing file
        // and a missing folder alike; CreateFile tells them apart, and so does this.
        OpenError OpenErrorOf(int error, const std::filesystem::path& path)
        {
            if (error == EEXIST)
                return OpenError::Exists;
            if (error == ENOTDIR)
                return OpenError::PathNotFound;
            if (error == ENOENT)
            {
                std::error_code status;
                bool folder = std::filesystem::is_directory(path.parent_path().empty() ? "." : path.parent_path(), status);
                return folder ? OpenError::Other : OpenError::PathNotFound;
            }
            return OpenError::Other;
        }
    }

    std::FILE* OpenFile(const std::filesystem::path& path, const char* mode)
    {
        return std::fopen(path.c_str(), mode);
    }

    std::vector<uint8_t> ReadWholeFile(const std::filesystem::path& path)
    {
        std::vector<uint8_t> data;
        std::FILE* file = OpenFile(path, "rb");
        if (file == nullptr)
            return data;
        if (fseeko(file, 0, SEEK_END) == 0)
        {
            off_t size = ftello(file);
            if (size > 0 && fseeko(file, 0, SEEK_SET) == 0)
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
            close(int(handle_));
    }

    File::File(File&& other) noexcept : handle_(std::exchange(other.handle_, -1)) {}

    File& File::operator=(File&& other) noexcept
    {
        if (this != &other)
        {
            if (IsOpen())
                close(int(handle_));
            handle_ = std::exchange(other.handle_, -1);
        }
        return *this;
    }

    File File::Open(const std::filesystem::path& path, bool write, Disposition disposition, OpenError& error)
    {
        int flags = (write ? O_RDWR : O_RDONLY) | O_CLOEXEC;
        switch (disposition)
        {
        case Disposition::OpenExisting: break;
        case Disposition::CreateNew: flags |= O_CREAT | O_EXCL; break;
        case Disposition::OpenAlways: flags |= O_CREAT; break;
        case Disposition::TruncateExisting: flags |= O_TRUNC; break;
        case Disposition::CreateAlways: flags |= O_CREAT | O_TRUNC; break;
        }
        int descriptor;
        do
            descriptor = open(path.c_str(), flags, 0666);
        while (descriptor == -1 && errno == EINTR);
        if (descriptor == -1)
        {
            error = OpenErrorOf(errno, path);
            return File();
        }
        // CreateFile refuses a folder; open takes one for reading.
        struct stat status;
        if (fstat(descriptor, &status) != 0 || S_ISDIR(status.st_mode))
        {
            close(descriptor);
            error = OpenError::Other;
            return File();
        }
        error = OpenError::None;
        return File(descriptor);
    }

    uint32_t File::Read(uint64_t offset, void* data, uint32_t length, uint32_t& done)
    {
        // A read returns less than asked only at the end of the file or when interrupted: go on until
        // the end, as ReadFile does.
        done = 0;
        while (done < length)
        {
            ssize_t count = pread(int(handle_), static_cast<uint8_t*>(data) + done, length - done, off_t(offset + done));
            if (count == 0)
                break;
            if (count < 0)
            {
                if (errno == EINTR)
                    continue;
                return uint32_t(errno);
            }
            done += uint32_t(count);
        }
        return 0;
    }

    uint32_t File::Write(uint64_t offset, const void* data, uint32_t length, uint32_t& done)
    {
        done = 0;
        while (done < length)
        {
            ssize_t count = pwrite(int(handle_), static_cast<const uint8_t*>(data) + done, length - done,
                off_t(offset + done));
            if (count < 0)
            {
                if (errno == EINTR)
                    continue;
                return uint32_t(errno);
            }
            // Nothing written and no error: the file system takes no more; do not ask again forever.
            if (count == 0)
                return uint32_t(EIO);
            done += uint32_t(count);
        }
        return 0;
    }

    bool File::Resize(uint64_t size)
    {
        return size <= uint64_t(LLONG_MAX) && ftruncate(int(handle_), off_t(size)) == 0;
    }

    uint64_t FileTime(std::filesystem::file_time_type time)
    {
        // A failed query gives the minimum, which Windows stores as its raw count, INT64_MIN; the
        // conversion below would overflow on it.
        if (time == std::filesystem::file_time_type::min())
            return 0x8000000000000000ull;
        using Intervals = std::chrono::duration<int64_t, std::ratio<1, 10'000'000>>;
        auto system = std::chrono::file_clock::to_sys(time);
        return uint64_t(std::chrono::duration_cast<Intervals>(system.time_since_epoch()).count()) + kUnixEpochIn1601;
    }
}
