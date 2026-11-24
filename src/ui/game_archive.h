#pragma once

// The game's interface archive, Naruto_clip.SRA: its GFx movies and their DDS images, by name.
//
// The container is the game's FRSA (reader 82641730; format read by EngineSrc's Tools/JadeRE,
// jadere/sra.py): a 24-byte header, table A of blobs (type, key, offset, stored size, size), table B
// naming them. Main thread only.

#include "vfs/vfs.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ui
{
    class GameArchive
    {
    public:
        // Entry types of table A.
        static constexpr uint32_t kMovie = 1; // CLIP
        static constexpr uint32_t kImage = 2; // DDS

        // Reads the tables of the file at path of the game's files, which must outlive this archive.
        // False when the file is missing or not an FRSA archive (logged).
        bool Open(const vfs::Source& game, const std::string& path);
        bool IsOpen() const { return file_ != nullptr; }

        // The entry's data, inflated. False when there is no such entry or it cannot be read (logged).
        bool Read(const std::string& name, std::vector<uint8_t>& out);
        // The names of the entries of a type, in no order.
        std::vector<std::string> Names(uint32_t type) const;

    private:
        struct Entry
        {
            uint32_t type = 0;
            uint32_t offset = 0;
            uint32_t storedSize = 0;
            uint32_t size = 0; // 0: stored as is
        };

        // Reads exactly size bytes at offset. False on an error or a short read.
        bool ReadAt(uint64_t offset, void* data, size_t size);

        std::unique_ptr<vfs::File> file_;
        std::unordered_map<std::string, Entry> entries_;
    };
}
