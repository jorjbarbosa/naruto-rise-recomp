// Host folders with the guest's name matching, and the extracted game as a source (folder.h).

#include "vfs/folder.h"

#include "ascii.h"
#include "host/files.h"
#include "host/paths.h"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace fs = std::filesystem;

namespace vfs
{
    namespace
    {
        // Whether a host file name (in the system's own units, so no conversion can throw) is name with
        // A-Z and a-z taken as the same; every other unit must be the same.
        bool SameNameNoCase(const fs::path::string_type& host, const std::string& name)
        {
            if (host.size() != name.size())
                return false;
            for (size_t i = 0; i < name.size(); i++)
            {
                auto unit = static_cast<std::make_unsigned_t<fs::path::value_type>>(host[i]);
                auto byte = static_cast<unsigned char>(name[i]);
                if (unit != byte && (unit > 0x7F || ascii::ToLower(char(unit)) != ascii::ToLower(char(byte))))
                    return false;
            }
            return true;
        }

        // The last write time of a host entry, 0 when the system does not give it.
        uint64_t HostTime(const fs::path& path)
        {
            std::error_code error;
            fs::file_time_type time = fs::last_write_time(path, error);
            return error ? 0 : host::FileTime(time);
        }

        class FolderFile final : public File
        {
        public:
            explicit FolderFile(host::File file) : file_(std::move(file)) {}

            uint32_t Read(uint64_t offset, void* data, uint32_t length, uint32_t& done) override
            {
                return file_.Read(offset, data, length, done);
            }

        private:
            host::File file_;
        };

        class FolderSource final : public Source
        {
        public:
            explicit FolderSource(fs::path root) : root_(std::move(root)) {}

            bool Find(std::string_view path, Entry& entry) const override
            {
                return StatHostPath(AppendPath(root_, path), entry);
            }

            bool List(std::string_view path, std::vector<Entry>& entries) const override
            {
                return ListHostFolder(AppendPath(root_, path), entries);
            }

            std::unique_ptr<File> Open(std::string_view path) const override
            {
                fs::path hostPath = AppendPath(root_, path);
                Entry entry;
                if (!StatHostPath(hostPath, entry) || entry.directory)
                    return nullptr;
                host::OpenError error;
                host::File file = host::File::Open(hostPath, false, host::Disposition::OpenExisting, error);
                if (!file.IsOpen())
                    return nullptr;
                return std::make_unique<FolderFile>(std::move(file));
            }

            std::string Describe() const override
            {
                return "folder " + host::PathText(root_);
            }

        private:
            fs::path root_;
        };
    }

    bool FindEntry(const fs::path& dir, const std::string& name, fs::path& entry)
    {
        std::error_code error;
        entry = dir / name;
        if (fs::exists(entry, error))
            return true;
        bool found = false;
        fs::directory_iterator it(dir, error);
        for (; !error && it != fs::directory_iterator(); it.increment(error))
        {
            const fs::path& candidate = it->path();
            if (SameNameNoCase(candidate.filename().native(), name) && (!found || candidate < entry))
            {
                entry = candidate;
                found = true;
            }
        }
        return found;
    }

    fs::path AppendPath(fs::path dir, std::string_view relative)
    {
        bool exists = true;
        size_t begin = 0;
        while (begin < relative.size())
        {
            size_t end = std::min(relative.find_first_of("\\/", begin), relative.size());
            if (end > begin)
            {
                std::string name(relative.substr(begin, end - begin));
                if (exists)
                {
                    fs::path entry;
                    exists = FindEntry(dir, name, entry);
                    dir = std::move(entry);
                }
                else
                {
                    dir /= name;
                }
            }
            begin = end + 1;
        }
        return dir;
    }

    bool StatHostPath(const fs::path& path, Entry& entry)
    {
        std::error_code error;
        fs::file_status status = fs::status(path, error);
        if (error || !fs::exists(status))
            return false;
        entry.name = host::PathText(path.filename());
        entry.directory = fs::is_directory(status);
        uint64_t size = entry.directory ? 0 : fs::file_size(path, error);
        entry.size = error ? 0 : size;
        entry.time = HostTime(path);
        return true;
    }

    bool ListHostFolder(const fs::path& dir, std::vector<Entry>& entries)
    {
        entries.clear();
        std::error_code error;
        fs::directory_iterator it(dir, error);
        if (error)
            return false;
        for (; !error && it != fs::directory_iterator(); it.increment(error))
        {
            Entry entry;
            std::error_code entryError;
            entry.name = host::PathText(it->path().filename());
            entry.directory = it->is_directory(entryError);
            uint64_t size = entry.directory ? 0 : it->file_size(entryError);
            entry.size = entryError ? 0 : size;
            entry.time = HostTime(it->path());
            entries.push_back(std::move(entry));
        }
        return true;
    }

    std::unique_ptr<Source> OpenFolder(const fs::path& dir)
    {
        return std::make_unique<FolderSource>(dir);
    }
}
