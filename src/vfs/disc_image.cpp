// Xbox 360 disc images read in place (disc_image.h).

#include "vfs/disc_image.h"

#include "host/files.h"
#include "host/paths.h"
#include "vfs/tree.h"

#include <cstdio>
#include <cstring>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace vfs
{
    namespace
    {
        constexpr uint64_t kSectorSize = 2048;
        // The volume descriptor, from the start of the partition: the magic, the root directory's
        // sector and size, the volume's time and the magic again at its end.
        constexpr uint64_t kVolumeOffset = 32 * kSectorSize;
        constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
        constexpr size_t kMagicSize = sizeof(kMagic) - 1;
        constexpr size_t kTrailingMagic = 0x7EC;
        // Where the file system's partition starts: an XISO rebuilt from the files (extract-xiso), and
        // the XGD2 disc every retail copy of this game is pressed on.
        constexpr uint64_t kPartitionOffsets[] = { 0, 0xFD90000 };

        // A directory table: entries of 14 bytes and the name, 4-byte aligned, as nodes of a binary
        // tree sorted by name; left and right are 4-byte offsets in the table, 0 for none. A table
        // whose first entry reads 0xFFFF on both sides is empty (the padding of a sector).
        constexpr size_t kEntryHeaderSize = 14;
        constexpr uint16_t kNoEntry = 0xFFFF;
        constexpr uint8_t kAttributeDirectory = 0x10;
        // Well above any disc's tables (this game's largest is 4 KB); a bigger one is damage.
        constexpr uint32_t kMaxTableSize = 16 * 1024 * 1024;

        uint16_t Read16(const uint8_t* p)
        {
            return uint16_t(p[0] | p[1] << 8);
        }

        uint32_t Read32(const uint8_t* p)
        {
            return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        }

        // Reads exactly length bytes at offset. False on an error or a short read.
        bool ReadExact(host::File& file, uint64_t offset, void* data, uint32_t length)
        {
            uint32_t done = 0;
            return file.Read(offset, data, length, done) == 0 && done == length;
        }

        class DiscFile final : public File
        {
        public:
            DiscFile(host::File image, uint64_t start, uint64_t size)
                : image_(std::move(image)), start_(start), size_(size) {}

            uint32_t Read(uint64_t offset, void* data, uint32_t length, uint32_t& done) override
            {
                done = 0;
                if (offset >= size_)
                    return 0;
                if (length > size_ - offset)
                    length = uint32_t(size_ - offset);
                return image_.Read(start_ + offset, data, length, done);
            }

        private:
            host::File image_; // the image's own handle for this file
            uint64_t start_;   // the file's first byte in the image
            uint64_t size_;
        };

        class DiscImageSource final : public Source
        {
        public:
            explicit DiscImageSource(fs::path path) : path_(std::move(path)) {}

            // Reads the volume descriptor and every directory table. False with the reason in why.
            bool Load(std::string& why);

            bool Find(std::string_view path, Entry& entry) const override
            {
                return tree_.Find(path, entry);
            }

            bool List(std::string_view path, std::vector<Entry>& entries) const override
            {
                return tree_.List(path, entries);
            }

            std::unique_ptr<File> Open(std::string_view path) const override
            {
                const Tree::Node* node = tree_.At(path);
                if (node == nullptr || node->directory)
                    return nullptr;
                host::OpenError error;
                host::File image = host::File::Open(path_, false, host::Disposition::OpenExisting, error);
                if (!image.IsOpen())
                    return nullptr;
                return std::make_unique<DiscFile>(std::move(image), node->offset, node->size);
            }

            std::string Describe() const override
            {
                char details[64];
                std::snprintf(details, sizeof(details), " (partition at %llX, %zu files)",
                    (unsigned long long)partition_, tree_.Files());
                return "disc image " + host::PathText(path_) + details;
            }

        private:
            // One entry of a directory table.
            struct TableEntry
            {
                uint16_t left = 0;
                uint16_t right = 0;
                uint32_t sector = 0;
                uint32_t size = 0;
                uint8_t attributes = 0;
                std::string_view name;
            };

            // A folder whose table is still to be read.
            struct PendingFolder
            {
                uint32_t index = 0; // in tree_
                uint32_t sector = 0;
                uint32_t size = 0;
            };

            bool ParseEntry(const std::vector<uint8_t>& table, uint32_t offset, TableEntry& entry,
                std::string& why) const;
            bool LoadTable(host::File& image, const PendingFolder& folder, std::vector<PendingFolder>& pending,
                std::string& why);

            fs::path path_;
            uint64_t imageSize_ = 0;
            uint64_t partition_ = 0;
            Tree tree_; // file offsets are bytes of the image; times are the volume's
            std::unordered_set<uint32_t> tables_; // sectors of the tables with entries read, against loops
        };

        bool DiscImageSource::Load(std::string& why)
        {
            std::error_code error;
            imageSize_ = fs::file_size(path_, error);
            host::OpenError openError;
            host::File image = host::File::Open(path_, false, host::Disposition::OpenExisting, openError);
            if (error || !image.IsOpen())
            {
                why = "cannot be read";
                return false;
            }

            uint8_t volume[kSectorSize];
            bool found = false;
            for (uint64_t partition : kPartitionOffsets)
            {
                if (ReadExact(image, partition + kVolumeOffset, volume, sizeof(volume)) &&
                    std::memcmp(volume, kMagic, kMagicSize) == 0 &&
                    std::memcmp(volume + kTrailingMagic, kMagic, kMagicSize) == 0)
                {
                    partition_ = partition;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                why = "not a game folder, a XEX, an Xbox 360 disc image or a ZArchive";
                return false;
            }
            uint32_t rootSector = Read32(volume + 20);
            uint32_t rootSize = Read32(volume + 24);
            tree_.SetTime(uint64_t(Read32(volume + 28)) | uint64_t(Read32(volume + 32)) << 32);

            // A work list rather than recursion, so a deep tree of folders cannot exhaust the host stack.
            std::vector<PendingFolder> pending{ PendingFolder{ Tree::kRoot, rootSector, rootSize } };
            while (!pending.empty())
            {
                PendingFolder folder = std::move(pending.back());
                pending.pop_back();
                if (!LoadTable(image, folder, pending, why))
                    return false;
            }
            return true;
        }

        bool DiscImageSource::ParseEntry(const std::vector<uint8_t>& table, uint32_t offset, TableEntry& entry,
            std::string& why) const
        {
            size_t at = size_t(offset) * 4;
            if (at + kEntryHeaderSize > table.size())
            {
                why = "a directory entry lies outside its table";
                return false;
            }
            const uint8_t* p = table.data() + at;
            entry.left = Read16(p);
            entry.right = Read16(p + 2);
            entry.sector = Read32(p + 4);
            entry.size = Read32(p + 8);
            entry.attributes = p[12];
            uint8_t nameSize = p[13];
            if (nameSize == 0 || at + kEntryHeaderSize + nameSize > table.size())
            {
                why = "a directory entry has a bad name";
                return false;
            }
            entry.name = std::string_view(reinterpret_cast<const char*>(p + kEntryHeaderSize), nameSize);
            return true;
        }

        // Reads the table of a folder into its children and adds its own folders to pending. The tree
        // is walked in order (left, node, right) with an explicit stack, so the children come sorted
        // and a deep tree cannot exhaust the host stack.
        bool DiscImageSource::LoadTable(host::File& image, const PendingFolder& folder,
            std::vector<PendingFolder>& pending, std::string& why)
        {
            uint32_t sector = folder.sector;
            uint32_t size = folder.size;
            if (size == 0)
                return true;
            uint64_t tableOffset = partition_ + uint64_t(sector) * kSectorSize;
            if (size < kEntryHeaderSize)
            {
                why = "a directory table is too short";
                return false;
            }
            if (size > kMaxTableSize || tableOffset + size > imageSize_)
            {
                why = "a directory table lies outside the image (cut short?)";
                return false;
            }
            std::vector<uint8_t> table(size);
            if (!ReadExact(image, tableOffset, table.data(), size))
            {
                why = "a directory table cannot be read";
                return false;
            }
            // Empty folders may share one padding sector, so only tables with entries count as read.
            if (Read16(table.data()) == kNoEntry && Read16(table.data() + 2) == kNoEntry)
                return true;
            if (!tables_.insert(sector).second)
            {
                why = "a directory contains itself";
                return false;
            }

            std::vector<TableEntry> stack;
            std::unordered_set<uint32_t> seen;
            uint32_t next = 0;
            bool descend = true;
            while (descend || !stack.empty())
            {
                while (descend)
                {
                    if (!seen.insert(next).second)
                    {
                        why = "a directory table loops";
                        return false;
                    }
                    TableEntry entry;
                    if (!ParseEntry(table, next, entry, why))
                        return false;
                    stack.push_back(entry);
                    descend = entry.left != 0;
                    next = entry.left;
                }
                TableEntry entry = stack.back();
                stack.pop_back();
                descend = entry.right != 0;
                next = entry.right;

                bool directory = (entry.attributes & kAttributeDirectory) != 0;
                uint64_t offset = partition_ + uint64_t(entry.sector) * kSectorSize;
                if (!directory && offset + entry.size > imageSize_)
                {
                    why = "the file " + std::string(entry.name) + " lies outside the image (cut short?)";
                    return false;
                }
                uint32_t child = tree_.Add(folder.index, std::string(entry.name), directory, offset, entry.size);
                if (child != Tree::kNone && directory)
                    pending.push_back(PendingFolder{ child, entry.sector, entry.size });
            }
            return true;
        }
    }

    std::unique_ptr<Source> OpenDiscImage(const fs::path& path, std::string& why)
    {
        auto source = std::make_unique<DiscImageSource>(path);
        if (!source->Load(why))
            return nullptr;
        return source;
    }
}
