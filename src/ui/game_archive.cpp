// Naruto_clip.SRA (game_archive.h).

#include "ui/game_archive.h"

#include "host/paths.h"
#include "log.h"

#include <zlib.h>

#include <cstring>

namespace ui
{
    namespace
    {
        constexpr size_t kHeaderSize = 24;
        constexpr size_t kTableAEntry = 28; // type, blob key, hash, offset, stored size, size, flag
        constexpr size_t kNameSize = 128;   // table B, kind 1

        uint32_t Read32(const uint8_t* p)
        {
            uint32_t value;
            std::memcpy(&value, p, 4);
            return value;
        }
    }

    bool GameArchive::Open(const std::filesystem::path& path)
    {
        file_.open(path, std::ios::binary);
        if (!file_)
        {
            Log("ui: no %s, the menus have no game images", host::PathText(path).c_str());
            return false;
        }
        auto fail = [&](const char* why) {
            Log("ui: error: %s: %s", host::PathText(path).c_str(), why);
            file_.close();
            entries_.clear();
            return false;
        };

        file_.seekg(0, std::ios::end);
        uint64_t fileSize = uint64_t(file_.tellg());
        uint8_t header[kHeaderSize];
        file_.seekg(0);
        if (!file_.read(reinterpret_cast<char*>(header), kHeaderSize) || std::memcmp(header, "FRSA", 4) != 0)
            return fail("not an SRA archive");
        uint32_t tableA = Read32(header + 8), countA = Read32(header + 12);
        uint32_t tableB = Read32(header + 16), countB = Read32(header + 20);
        if (uint64_t(tableA) + uint64_t(countA) * kTableAEntry != tableB || tableB > fileSize)
            return fail("unexpected layout");

        // Both tables, to the end of the file.
        std::vector<uint8_t> tables(size_t(fileSize - tableA));
        file_.seekg(tableA);
        if (!file_.read(reinterpret_cast<char*>(tables.data()), std::streamsize(tables.size())))
            return fail("cannot read the tables");

        std::unordered_map<uint32_t, Entry> byKey;
        for (uint32_t i = 0; i < countA; i++)
        {
            const uint8_t* p = tables.data() + size_t(i) * kTableAEntry;
            Entry entry{ Read32(p), Read32(p + 12), Read32(p + 16), Read32(p + 20) };
            if (uint64_t(entry.offset) + entry.storedSize > tableA)
                return fail("a blob runs past its area");
            byKey[Read32(p + 4)] = entry;
        }
        size_t at = size_t(tableB - tableA);
        for (uint32_t i = 0; i < countB; i++)
        {
            if (at + 12 > tables.size())
                return fail("table B runs past the end");
            uint32_t kind = Read32(tables.data() + at), key = Read32(tables.data() + at + 8);
            at += 12;
            size_t refSize = kind == 0 ? 4 : kind == 1 ? kNameSize : 0;
            if (refSize == 0 || at + refSize > tables.size())
                return fail("a bad table B entry");
            if (kind == 1)
            {
                const char* name = reinterpret_cast<const char*>(tables.data() + at);
                if (auto it = byKey.find(key); it != byKey.end())
                    entries_[std::string(name, strnlen(name, kNameSize))] = it->second;
            }
            at += refSize;
        }
        if (at != tables.size())
            return fail("bytes after table B");
        Log("ui: %s: %zu named entries", host::PathText(path.filename()).c_str(), entries_.size());
        return true;
    }

    bool GameArchive::Read(const std::string& name, std::vector<uint8_t>& out)
    {
        auto it = entries_.find(name);
        if (!file_.is_open() || it == entries_.end())
            return false;
        const Entry& entry = it->second;
        std::vector<uint8_t> stored(entry.storedSize);
        file_.clear();
        file_.seekg(entry.offset);
        if (!file_.read(reinterpret_cast<char*>(stored.data()), std::streamsize(stored.size())))
        {
            Log("ui: error: cannot read %s from the archive", name.c_str());
            return false;
        }
        // Size 0 is stored as is; so is an entry that does not start as a zlib stream (sra.py's test).
        if (entry.size == 0 || stored.empty() || stored[0] != 0x78)
        {
            out = std::move(stored);
            return true;
        }
        out.resize(entry.size);
        uLongf size = entry.size;
        if (uncompress(out.data(), &size, stored.data(), uLong(stored.size())) != Z_OK || size != entry.size)
        {
            Log("ui: error: %s does not inflate to its %u bytes", name.c_str(), entry.size);
            return false;
        }
        return true;
    }

    std::vector<std::string> GameArchive::Names(uint32_t type) const
    {
        std::vector<std::string> names;
        for (const auto& [name, entry] : entries_)
            if (entry.type == type)
                names.push_back(name);
        return names;
    }
}
