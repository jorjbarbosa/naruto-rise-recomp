// Mounting the game's files and reading whole files of a source (vfs.h).

#include "vfs/vfs.h"

#include "host/files.h"
#include "host/paths.h"
#include "log.h"
#include "vfs/disc_image.h"
#include "vfs/folder.h"
#include "vfs/zarchive.h"

#include <algorithm>
#include <cstring>

namespace fs = std::filesystem;

namespace vfs
{
    namespace
    {
        constexpr char kXexMagic[4] = { 'X', 'E', 'X', '2' };
        constexpr uint32_t kReadChunk = 64 * 1024 * 1024;

        // Whether the host file at path starts as a XEX.
        bool IsXex(const fs::path& path)
        {
            host::OpenError error;
            host::File file = host::File::Open(path, false, host::Disposition::OpenExisting, error);
            char magic[sizeof(kXexMagic)];
            uint32_t done = 0;
            return file.IsOpen() && file.Read(0, magic, sizeof(magic), done) == 0 && done == sizeof(magic) &&
                std::memcmp(magic, kXexMagic, sizeof(magic)) == 0;
        }
    }

    std::unique_ptr<Source> Mount(const fs::path& path)
    {
        std::error_code error;
        std::unique_ptr<Source> source;
        if (fs::is_directory(path, error))
        {
            source = OpenFolder(path);
        }
        else if (!fs::is_regular_file(path, error))
        {
            Log("game: %s does not exist", host::PathText(path).c_str());
            return nullptr;
        }
        else if (IsXex(path))
        {
            source = OpenFolder(path.parent_path());
        }
        else
        {
            // A ZArchive is known by its footer's magic; anything else may be a disc image.
            std::string why;
            source = OpenZArchive(path, why);
            if (source == nullptr && why.empty())
                source = OpenDiscImage(path, why);
            if (source == nullptr)
            {
                Log("game: %s: %s", host::PathText(path).c_str(), why.c_str());
                return nullptr;
            }
        }

        Entry entry;
        if (!source->Find("default.xex", entry) || entry.directory)
        {
            Log("game: no default.xex in the %s", source->Describe().c_str());
            return nullptr;
        }
        Log("game: %s", source->Describe().c_str());
        return source;
    }

    bool ReadWholeFile(const Source& source, std::string_view path, std::vector<uint8_t>& data)
    {
        data.clear();
        Entry entry;
        if (!source.Find(path, entry) || entry.directory)
            return false;
        std::unique_ptr<File> file = source.Open(path);
        if (file == nullptr)
            return false;
        data.resize(size_t(entry.size));
        for (uint64_t offset = 0; offset < entry.size;)
        {
            uint32_t length = uint32_t(std::min<uint64_t>(kReadChunk, entry.size - offset));
            uint32_t done = 0;
            if (file->Read(offset, data.data() + offset, length, done) != 0 || done != length)
            {
                data.clear();
                return false;
            }
            offset += done;
        }
        return true;
    }
}
