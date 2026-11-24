#pragma once

// The game's files: one read-only source mounted at startup, an extracted folder, an Xbox 360 disc
// image or a ZArchive, read the way the console reads its disc: paths relative to the root with '\'
// (or '/') between names, names matched regardless of ASCII case.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vfs
{
    // A file or folder of a source or of a host folder.
    struct Entry
    {
        std::string name;       // as stored: "naruto.bf" when "NARUTO.BF" was asked
        bool directory = false;
        uint64_t size = 0;      // 0 for a folder
        uint64_t time = 0;      // last write, 100 ns intervals since 1601-01-01 UTC; 0 when unknown
    };

    // What File::Read returns when the source's own data is damaged, such as a block of an archive that
    // does not decompress; no system's error code has this value.
    constexpr uint32_t kErrorDamaged = 0xFFFFFFFF;

    // An open file of a source. One call at a time (the caller locks); separate Files are independent.
    class File
    {
    public:
        virtual ~File() = default;

        // Reads up to length bytes at offset into data; done is what was read, short only at the end
        // of the file. 0 on success (reaching the end included), otherwise the system's error code or
        // kErrorDamaged.
        virtual uint32_t Read(uint64_t offset, void* data, uint32_t length, uint32_t& done) = 0;
    };

    // A read-only tree of files. Every call may run on any thread at any time; none of them throws
    // except for an allocation failure. A disc image or archive does not change after it is opened;
    // a folder is read from the disk at each call, so a file changed between two calls (a Find and an
    // Open) may differ between them.
    class Source
    {
    public:
        virtual ~Source() = default;

        // The entry at path ("" is the root). False when nothing is there.
        virtual bool Find(std::string_view path, Entry& entry) const = 0;

        // The entries of the folder at path, in the source's order. False when path is not a folder.
        virtual bool List(std::string_view path, std::vector<Entry>& entries) const = 0;

        // The file at path opened for reading, or null when it is missing, a folder or cannot be
        // opened.
        virtual std::unique_ptr<File> Open(std::string_view path) const = 0;

        // What the source is, for the log: "folder <path>", "disc image <path> (...)", "ZArchive
        // <path> (...)".
        virtual std::string Describe() const = 0;
    };

    // Mounts the game at path, decided by the content and not by the name: a folder, the path of a
    // XEX (its folder), an Xbox 360 disc image (a dump of the disc, or an XISO rebuilt from its files)
    // or a ZArchive. Null when path is none of them or holds no default.xex (logged).
    std::unique_ptr<Source> Mount(const std::filesystem::path& path);

    // The whole file at path of source into data. False when it is missing, a folder or cannot be
    // read.
    bool ReadWholeFile(const Source& source, std::string_view path, std::vector<uint8_t>& data);
}
