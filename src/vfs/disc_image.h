#pragma once

// Xbox 360 disc images: the XDVDFS file system of the game partition, read in place.

#include "vfs/vfs.h"

#include <filesystem>
#include <memory>
#include <string>

namespace vfs
{
    // The disc image at path as a source: its directory tree is read here, once; files are read from
    // the image at each call. Null with the reason in why when path is not a disc image (why says so)
    // or the image is damaged or cut short.
    std::unique_ptr<Source> OpenDiscImage(const std::filesystem::path& path, std::string& why);
}
