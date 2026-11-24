#pragma once

// ZArchive (.zar): a folder packed as one stream of 64 KiB blocks, each compressed alone with zstd, and
// a tree of names, read in place. The format is ZArchive's (github.com/Exzap/ZArchive, MIT No
// Attribution), the archive Xenia and Cemu read their games from; this reader is our own.

#include "vfs/vfs.h"

#include <filesystem>
#include <memory>
#include <string>

namespace vfs
{
    // The ZArchive at path as a source: its tree is read here, once; files are read and decompressed
    // from the archive at each call, through a cache of recent blocks that the archive's open files
    // share. Null when path is not a ZArchive (why empty), or with the reason in why when the archive is
    // damaged or cut short.
    std::unique_ptr<Source> OpenZArchive(const std::filesystem::path& path, std::string& why);
}
