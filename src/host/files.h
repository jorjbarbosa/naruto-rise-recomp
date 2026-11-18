#pragma once

// Host file access: C streams, whole-file reads, files opened for positional reads and writes, and
// host file times.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace host
{
    // The file at path opened as a C stream with mode (fopen's modes, ASCII), or null when it cannot
    // be opened. Takes the path in the OS's own encoding, so any name the OS allows works. The caller
    // closes the stream. Any thread; does not throw except for an allocation failure.
    std::FILE* OpenFile(const std::filesystem::path& path, const char* mode);

    // The whole file at path, or nothing when it is missing, empty or cannot be read. Any thread;
    // does not throw except for an allocation failure.
    std::vector<uint8_t> ReadWholeFile(const std::filesystem::path& path);

    // What File::Open does when the file exists and when it does not (CreateFile's dispositions,
    // which are also the guest's).
    enum class Disposition
    {
        OpenExisting,     // open; fail when missing
        CreateNew,        // create; fail when it exists
        OpenAlways,       // open, or create when missing
        TruncateExisting, // open and empty it; fail when missing
        CreateAlways,     // create, or empty it when it exists
    };

    // Why File::Open failed.
    enum class OpenError
    {
        None,
        PathNotFound, // a folder of the path is missing
        Exists,       // CreateNew on a file that exists
        Other,        // anything else, a missing file among them
    };

    // A host file read and written at given offsets: there is no file position, so the caller keeps
    // its own. Other openers may read, write and delete the file meanwhile. Move-only; the destructor
    // closes it. One call at a time per File (the caller locks); separate Files are independent.
    // Read, Write and Resize are only for an open File.
    class File
    {
    public:
        File() = default;
        ~File();
        File(File&& other) noexcept;
        File& operator=(File&& other) noexcept;
        File(const File&) = delete;
        File& operator=(const File&) = delete;

        // Opens path for reading and, when write is set, writing. A closed File on failure, with the
        // reason in error.
        static File Open(const std::filesystem::path& path, bool write, Disposition disposition, OpenError& error);

        bool IsOpen() const { return handle_ != -1; }

        // Reads up to length bytes at offset into data; done is what was read, short at the end of the
        // file. 0 on success (reaching the end of the file included), otherwise the system's error code.
        uint32_t Read(uint64_t offset, void* data, uint32_t length, uint32_t& done);

        // Writes length bytes of data at offset, growing the file as needed; done is what was written.
        // 0 on success, otherwise the system's error code.
        uint32_t Write(uint64_t offset, const void* data, uint32_t length, uint32_t& done);

        // Sets the file's size to size, cutting or zero-extending it. False on failure.
        bool Resize(uint64_t size);

    private:
        explicit File(intptr_t handle) : handle_(handle) {}

        intptr_t handle_ = -1; // the OS's handle or descriptor; -1 when closed
    };

    // A file time from std::filesystem as 100 ns intervals since 1601-01-01 UTC, the file times of the
    // Xbox kernel.
    uint64_t FileTime(std::filesystem::file_time_type time);
}
