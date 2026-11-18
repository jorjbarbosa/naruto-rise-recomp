#pragma once

// Host paths of the runtime.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace host
{
    // The folder of the running executable, or an empty path when the OS cannot give it (logged
    // once). Any thread; does not throw.
    std::filesystem::path ExecutableDirectory();

    // A path as UTF-8, for the log. Does not throw (a path the conversion rejects gives "?").
    std::string PathText(const std::filesystem::path& path);

    // The whole file at path, or nothing when it is missing, empty or cannot be read. Any thread;
    // does not throw except for an allocation failure.
    std::vector<uint8_t> ReadWholeFile(const std::filesystem::path& path);
}
