// ZArchive files read in place, with a block cache shared by the archive's open files (zarchive.h).

#include "vfs/zarchive.h"

#include "host/files.h"
#include "host/paths.h"
#include "vfs/folder.h"
#include "vfs/tree.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <list>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <zstd.h>

namespace fs = std::filesystem;

namespace vfs
{
    namespace
    {
        // Every value of the format is big-endian.
        //
        // The footer, the archive's last 144 bytes: six sections as {u64 offset, u64 size} (the
        // compressed data, the offset records, the names, the tree and two meta sections no writer
        // fills), a SHA-256 of the archive (not checked: the blocks fail to decompress when damaged),
        // the archive's size, the version and the magic.
        constexpr size_t kFooterSize = 144;
        constexpr size_t kSectionCount = 6;
        constexpr size_t kTotalSizeAt = 128;
        constexpr size_t kVersionAt = 136;
        constexpr size_t kMagicAt = 140;
        constexpr uint32_t kVersion = 0x61BF3A01;
        constexpr uint32_t kMagic = 0x169F52D6;
        enum Section
        {
            kData,
            kOffsetRecords,
            kNames,
            kTree,
        };

        // The data: the packed files as one stream cut into blocks, the last one padded with zeros. A
        // block stored at full size is raw. One offset record per 16 blocks: the first one's offset in
        // the data section and each one's stored size - 1.
        constexpr uint32_t kBlockSize = 64 * 1024;
        constexpr uint32_t kBlocksPerRecord = 16;
        constexpr size_t kRecordSize = 8 + 2 * kBlocksPerRecord;

        // The tree: 16-byte nodes, the root first, each folder's entries a run of nodes after it
        // (sorted by name). A node is the name's offset in the name table with the top bit set for a
        // file, then for a file its offset in the data and its size (48 bits each: the low 32 bits of
        // each, then the high 16 bits of the size and of the offset in one word), for a folder the
        // index of its first entry and their count. A name is its length in one byte (1 to 127), or in
        // two bytes when the first one's top bit is set (the low 7 bits, then the high 8), and the bytes.
        constexpr size_t kNodeSize = 16;
        constexpr uint32_t kFileFlag = 0x80000000;
        constexpr uint8_t kLongNameFlag = 0x80;

        // Well above any game's tables; the names, the tree and the records are read whole.
        constexpr uint64_t kMaxTableSize = 256 * 1024 * 1024;
        // Blocks kept decompressed for all the archive's files: 4 MiB.
        constexpr size_t kCacheBlocks = 64;

        uint16_t Read16(const uint8_t* p)
        {
            return uint16_t(p[0] << 8 | p[1]);
        }

        uint32_t Read32(const uint8_t* p)
        {
            return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
        }

        uint64_t Read64(const uint8_t* p)
        {
            return uint64_t(Read32(p)) << 32 | Read32(p + 4);
        }

        // Reads exactly length bytes at offset. False on an error or a short read.
        bool ReadExact(host::File& file, uint64_t offset, void* data, uint32_t length)
        {
            uint32_t done = 0;
            return file.Read(offset, data, length, done) == 0 && done == length;
        }

        using Block = std::shared_ptr<const std::vector<uint8_t>>;

        struct DecompressorDeleter
        {
            void operator()(ZSTD_DCtx* context) const { ZSTD_freeDCtx(context); }
        };
        using Decompressor = std::unique_ptr<ZSTD_DCtx, DecompressorDeleter>;

        // The decompressed blocks the archive's files read most recently. Any thread: a mutex guards it,
        // held only to find, add or drop a block, never while one is read or decompressed, so files
        // read on separate threads (the loader, the videos, the sound) do not wait on each other's
        // decompression. Two threads that miss the same block both decompress it; the second one's
        // copy replaces nothing.
        class BlockCache
        {
        public:
            Block Get(uint64_t index)
            {
                std::lock_guard lock(mutex_);
                auto it = blocks_.find(index);
                if (it == blocks_.end())
                    return nullptr;
                order_.splice(order_.begin(), order_, it->second.second);
                return it->second.first;
            }

            void Put(uint64_t index, Block block)
            {
                std::lock_guard lock(mutex_);
                if (blocks_.count(index) != 0)
                    return;
                order_.push_front(index);
                blocks_.emplace(index, std::make_pair(std::move(block), order_.begin()));
                if (blocks_.size() > kCacheBlocks)
                {
                    blocks_.erase(order_.back());
                    order_.pop_back();
                }
            }

        private:
            std::mutex mutex_;
            std::list<uint64_t> order_; // block indices, the most recent first
            std::unordered_map<uint64_t, std::pair<Block, std::list<uint64_t>::iterator>> blocks_;
        };

        struct OffsetRecord
        {
            uint64_t base = 0;                      // the first block's offset in the data section
            uint16_t sizes[kBlocksPerRecord] = {};  // each block's stored size - 1
        };

        class ZArchiveSource final : public Source
        {
        public:
            explicit ZArchiveSource(fs::path path) : path_(std::move(path)) {}

            // Reads the footer, the offset records and the tree. False when the file is not a ZArchive
            // (why empty) or is damaged (the reason in why).
            bool Load(std::string& why);

            bool Find(std::string_view path, Entry& entry) const override
            {
                return tree_.Find(path, entry);
            }

            bool List(std::string_view path, std::vector<Entry>& entries) const override
            {
                return tree_.List(path, entries);
            }

            std::unique_ptr<File> Open(std::string_view path) const override;

            std::string Describe() const override
            {
                char details[32];
                std::snprintf(details, sizeof(details), " (%zu files)", tree_.Files());
                return "ZArchive " + host::PathText(path_) + details;
            }

            // The block at index, from the cache or read through archive and decompressed with
            // decompressor, both the calling file's own. Null with the system's error code, or
            // kErrorDamaged, in error.
            Block ReadBlock(uint64_t index, host::File& archive, ZSTD_DCtx* decompressor,
                std::vector<uint8_t>& stored, uint32_t& error) const;

        private:
            bool LoadTree(const std::vector<uint8_t>& names, const std::vector<uint8_t>& nodes, std::string& why);

            fs::path path_;
            uint64_t dataOffset_ = 0;
            uint64_t dataSize_ = 0;
            std::vector<OffsetRecord> records_;
            Tree tree_; // file offsets are bytes of the decompressed stream; times are the archive file's
            mutable BlockCache cache_;
        };

        // A file of the archive: its own handle on the archive, decompression context and buffer, so
        // that separate files read at the same time without locking anything but the cache.
        class ZArchiveFile final : public File
        {
        public:
            ZArchiveFile(const ZArchiveSource& source, host::File archive, Decompressor decompressor, uint64_t start,
                uint64_t size)
                : source_(source), archive_(std::move(archive)), decompressor_(std::move(decompressor)),
                  start_(start), size_(size) {}

            uint32_t Read(uint64_t offset, void* data, uint32_t length, uint32_t& done) override
            {
                done = 0;
                if (offset >= size_)
                    return 0;
                if (length > size_ - offset)
                    length = uint32_t(size_ - offset);
                auto* out = static_cast<uint8_t*>(data);
                uint64_t at = start_ + offset;
                while (done < length)
                {
                    uint32_t within = uint32_t(at % kBlockSize);
                    uint32_t step = std::min(length - done, kBlockSize - within);
                    uint32_t error = 0;
                    Block block = source_.ReadBlock(at / kBlockSize, archive_, decompressor_.get(), stored_, error);
                    if (block == nullptr)
                        return error;
                    std::memcpy(out + done, block->data() + within, step);
                    done += step;
                    at += step;
                }
                return 0;
            }

        private:
            const ZArchiveSource& source_; // outlives its files: the source lives for the whole run
            host::File archive_;
            Decompressor decompressor_;
            std::vector<uint8_t> stored_; // a compressed block as read
            uint64_t start_;              // the file's first byte in the decompressed stream
            uint64_t size_;
        };

        bool ZArchiveSource::Load(std::string& why)
        {
            std::error_code error;
            uint64_t fileSize = fs::file_size(path_, error);
            host::OpenError openError;
            host::File archive = host::File::Open(path_, false, host::Disposition::OpenExisting, openError);
            if (error || !archive.IsOpen())
            {
                why = "cannot be read";
                return false;
            }
            uint8_t footer[kFooterSize];
            if (fileSize < kFooterSize)
                return false; // not a ZArchive: why stays empty
            if (!ReadExact(archive, fileSize - kFooterSize, footer, sizeof(footer)))
            {
                why = "cannot be read";
                return false;
            }
            if (Read32(footer + kMagicAt) != kMagic || Read32(footer + kVersionAt) != kVersion)
                return false;
            if (Read64(footer + kTotalSizeAt) != fileSize)
            {
                why = "the archive's size is not the one its footer gives (cut short?)";
                return false;
            }
            uint64_t offsets[kSectionCount];
            uint64_t sizes[kSectionCount];
            for (size_t i = 0; i < kSectionCount; i++)
            {
                offsets[i] = Read64(footer + i * 16);
                sizes[i] = Read64(footer + i * 16 + 8);
                if (offsets[i] > fileSize || sizes[i] > fileSize - offsets[i])
                {
                    why = "a section lies outside the archive";
                    return false;
                }
            }
            if (sizes[kOffsetRecords] > kMaxTableSize || sizes[kNames] > kMaxTableSize || sizes[kTree] > kMaxTableSize)
            {
                why = "the archive's tables are too large";
                return false;
            }
            if (sizes[kOffsetRecords] == 0 || sizes[kOffsetRecords] % kRecordSize != 0 || sizes[kTree] == 0 ||
                sizes[kTree] % kNodeSize != 0)
            {
                why = "the archive's tables have a bad size";
                return false;
            }
            dataOffset_ = offsets[kData];
            dataSize_ = sizes[kData];

            std::vector<uint8_t> records(static_cast<size_t>(sizes[kOffsetRecords]));
            std::vector<uint8_t> names(static_cast<size_t>(sizes[kNames]));
            std::vector<uint8_t> nodes(static_cast<size_t>(sizes[kTree]));
            if (!ReadExact(archive, offsets[kOffsetRecords], records.data(), uint32_t(records.size())) ||
                !ReadExact(archive, offsets[kNames], names.data(), uint32_t(names.size())) ||
                !ReadExact(archive, offsets[kTree], nodes.data(), uint32_t(nodes.size())))
            {
                why = "the archive's tables cannot be read";
                return false;
            }
            records_.resize(records.size() / kRecordSize);
            for (size_t i = 0; i < records_.size(); i++)
            {
                const uint8_t* p = records.data() + i * kRecordSize;
                records_[i].base = Read64(p);
                for (uint32_t j = 0; j < kBlocksPerRecord; j++)
                    records_[i].sizes[j] = Read16(p + 8 + j * 2);
            }

            Entry archiveEntry;
            if (StatHostPath(path_, archiveEntry))
                tree_.SetTime(archiveEntry.time);
            return LoadTree(names, nodes, why);
        }

        // Adds the archive's tree to tree_, folder by folder from a work list, so a deep tree cannot
        // exhaust the host stack. Each folder's entries must come after it and belong to it alone, which
        // rules out loops.
        bool ZArchiveSource::LoadTree(const std::vector<uint8_t>& names, const std::vector<uint8_t>& nodes,
            std::string& why)
        {
            auto word = [&](uint32_t node, uint32_t index)
            {
                return Read32(nodes.data() + size_t(node) * kNodeSize + index * 4);
            };
            auto name = [&](uint32_t offset, std::string_view& text)
            {
                if (offset >= names.size())
                    return false;
                uint32_t length = names[offset] & ~kLongNameFlag;
                uint32_t start = offset + 1;
                if ((names[offset] & kLongNameFlag) != 0)
                {
                    if (start >= names.size())
                        return false;
                    length |= uint32_t(names[start]) << 7;
                    start++;
                }
                if (length == 0 || length > names.size() - start)
                    return false;
                text = std::string_view(reinterpret_cast<const char*>(names.data()) + start, length);
                return true;
            };

            uint32_t count = uint32_t(nodes.size() / kNodeSize);
            if ((word(0, 0) & kFileFlag) != 0)
            {
                why = "the archive's root is not a folder";
                return false;
            }
            // The decompressed stream's size: every file must lie in it.
            uint64_t streamSize = uint64_t(records_.size()) * kBlocksPerRecord * kBlockSize;
            std::vector<bool> reached(count);
            reached[0] = true;
            std::vector<std::pair<uint32_t, uint32_t>> pending{ { 0, Tree::kRoot } }; // archive node, tree node
            while (!pending.empty())
            {
                auto [folder, folderIndex] = pending.back();
                pending.pop_back();
                uint32_t first = word(folder, 1);
                uint32_t entries = word(folder, 2);
                if (entries == 0)
                    continue;
                if (first <= folder || first > count || entries > count - first)
                {
                    why = "a folder's entries lie outside the archive's tree";
                    return false;
                }
                for (uint32_t node = first; node < first + entries; node++)
                {
                    if (reached[node])
                    {
                        why = "an entry of the archive's tree belongs to two folders";
                        return false;
                    }
                    reached[node] = true;
                    uint32_t nameAndType = word(node, 0);
                    std::string_view text;
                    if (!name(nameAndType & ~kFileFlag, text))
                    {
                        why = "an entry of the archive's tree has a bad name";
                        return false;
                    }
                    bool directory = (nameAndType & kFileFlag) == 0;
                    uint32_t high = word(node, 3);
                    uint64_t offset = uint64_t(high & 0xFFFF) << 32 | word(node, 1);
                    uint64_t size = uint64_t(high & 0xFFFF0000) << 16 | word(node, 2);
                    if (!directory && (offset > streamSize || size > streamSize - offset))
                    {
                        why = "the file " + std::string(text) + " lies outside the archive's data";
                        return false;
                    }
                    uint32_t child = tree_.Add(folderIndex, std::string(text), directory, offset, size);
                    if (child != Tree::kNone && directory)
                        pending.emplace_back(node, child);
                }
            }
            return true;
        }

        std::unique_ptr<File> ZArchiveSource::Open(std::string_view path) const
        {
            const Tree::Node* node = tree_.At(path);
            if (node == nullptr || node->directory)
                return nullptr;
            host::OpenError error;
            host::File archive = host::File::Open(path_, false, host::Disposition::OpenExisting, error);
            Decompressor decompressor(ZSTD_createDCtx());
            if (!archive.IsOpen() || decompressor == nullptr)
                return nullptr;
            return std::make_unique<ZArchiveFile>(*this, std::move(archive), std::move(decompressor), node->offset,
                node->size);
        }

        Block ZArchiveSource::ReadBlock(uint64_t index, host::File& archive, ZSTD_DCtx* decompressor,
            std::vector<uint8_t>& stored, uint32_t& error) const
        {
            if (Block block = cache_.Get(index))
                return block;
            error = kErrorDamaged;
            uint64_t recordIndex = index / kBlocksPerRecord;
            if (recordIndex >= records_.size())
                return nullptr;
            const OffsetRecord& record = records_[size_t(recordIndex)];
            uint32_t within = uint32_t(index % kBlocksPerRecord);
            uint64_t offset = record.base;
            for (uint32_t i = 0; i < within; i++)
                offset += uint64_t(record.sizes[i]) + 1;
            uint32_t storedSize = uint32_t(record.sizes[within]) + 1;
            if (offset > dataSize_ || storedSize > dataSize_ - offset)
                return nullptr;

            // A raw block is read straight into the cached copy, a compressed one into stored.
            auto block = std::make_shared<std::vector<uint8_t>>(kBlockSize);
            uint8_t* target = block->data();
            if (storedSize != kBlockSize)
            {
                stored.resize(storedSize);
                target = stored.data();
            }
            uint32_t done = 0;
            if (uint32_t readError = archive.Read(dataOffset_ + offset, target, storedSize, done); readError != 0)
            {
                error = readError;
                return nullptr;
            }
            if (done != storedSize)
                return nullptr;
            if (storedSize != kBlockSize &&
                ZSTD_decompressDCtx(decompressor, block->data(), kBlockSize, stored.data(), storedSize) != kBlockSize)
                return nullptr;
            error = 0;
            cache_.Put(index, block);
            return block;
        }
    }

    std::unique_ptr<Source> OpenZArchive(const fs::path& path, std::string& why)
    {
        why.clear();
        auto source = std::make_unique<ZArchiveSource>(path);
        if (!source->Load(why))
            return nullptr;
        return source;
    }
}
