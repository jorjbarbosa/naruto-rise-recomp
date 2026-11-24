#pragma once

// Host folders seen as the guest sees its file systems: names matched regardless of ASCII case,
// relative paths with '\' (or '/') between names. Used for an extracted game (a read-only source) and
// for the folders of the data the game writes. Any thread; nothing here throws except for an
// allocation failure.

#include "vfs/vfs.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vfs
{
    // Sets entry to the entry of the host folder dir called name, matched as the guest's file systems
    // match names, regardless of ASCII case: the game asks for "naruto.BF" where the disc holds
    // "naruto.bf". The exact name when it exists (all a case-insensitive host file system needs), else
    // the folder's name that equals it but for case, the lowest when there are several so that the
    // choice never changes. False, with entry dir / name, when there is none.
    bool FindEntry(const std::filesystem::path& dir, const std::string& name, std::filesystem::path& entry);

    // dir followed by the relative path, each name matched to an existing entry by FindEntry; empty
    // names (doubled or leading separators) are skipped. From the first name that matches nothing on,
    // the names are kept as given: a file or folder to create, or one that does not exist.
    std::filesystem::path AppendPath(std::filesystem::path dir, std::string_view relative);

    // The entry of the host file or folder at path. False when there is none.
    bool StatHostPath(const std::filesystem::path& path, Entry& entry);

    // The entries of the host folder dir, in the system's order. False when it cannot be listed.
    bool ListHostFolder(const std::filesystem::path& dir, std::vector<Entry>& entries);

    // The extracted game in the host folder dir, as a source. Its files are read as they are on disk
    // at each call, so a change to the folder shows at once.
    std::unique_ptr<Source> OpenFolder(const std::filesystem::path& dir);
}
