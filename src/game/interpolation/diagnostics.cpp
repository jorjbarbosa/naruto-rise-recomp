// Output files of the state interpolation probes (diagnostics.h).

#include "game/interpolation/diagnostics.h"

#include "host/files.h"
#include "host/paths.h"
#include "log.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace game::interpolation
{
    std::FILE* OpenDiagnosticsFile(const char* name)
    {
        std::filesystem::path executable = host::ExecutableDirectory();
        if (executable.empty())
            return nullptr;
        std::filesystem::path folder = executable / "diagnostics";
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        std::filesystem::path path = folder / name;
        std::FILE* file = host::OpenFile(path, "w");
        Log(file != nullptr ? "diagnostics: writing %s" : "diagnostics: cannot open %s", host::PathText(path).c_str());
        return file;
    }

    bool EnvironmentFlag(const char* name)
    {
        const char* value = std::getenv(name);
        return value != nullptr && std::strcmp(value, "1") == 0;
    }

    bool EnvironmentOff(const char* name)
    {
        const char* value = std::getenv(name);
        return value != nullptr && std::strcmp(value, "0") == 0;
    }
}
