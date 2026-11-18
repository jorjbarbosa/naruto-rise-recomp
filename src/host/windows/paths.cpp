// Host paths of the runtime on Windows (paths.h).

#include "host/paths.h"

#include "log.h"

#include <windows.h>

namespace host
{
    std::filesystem::path ExecutableDirectory()
    {
        wchar_t executable[MAX_PATH];
        DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) // MAX_PATH: the path was cut short
        {
            LOG_ONCE("error: cannot find the executable's folder (error %lu)", GetLastError());
            return {};
        }
        return std::filesystem::path(executable).parent_path();
    }

    std::string PathText(const std::filesystem::path& path)
    {
        const std::wstring& wide = path.native();
        int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr);
        if (size <= 0 && !wide.empty())
            return "?";
        std::string text(size_t(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), text.data(), size, nullptr, nullptr);
        return text;
    }
}
