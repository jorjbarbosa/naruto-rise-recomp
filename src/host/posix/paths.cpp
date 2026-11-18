// Host paths of the runtime on POSIX (paths.h).

#include "host/paths.h"

#include "log.h"

#include <system_error>

namespace host
{
    std::filesystem::path ExecutableDirectory()
    {
        std::error_code error;
        std::filesystem::path executable = std::filesystem::read_symlink("/proc/self/exe", error);
        if (error)
        {
            LOG_ONCE("error: cannot find the executable's folder (%s)", error.message().c_str());
            return {};
        }
        return executable.parent_path();
    }

    std::string PathText(const std::filesystem::path& path)
    {
        // POSIX paths are bytes, and the system's are UTF-8.
        return path.native();
    }
}
