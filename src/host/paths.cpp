// Host paths of the runtime (paths.h).

#include "host/paths.h"

#include "log.h"

#include <cstdio>
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

    std::vector<uint8_t> ReadWholeFile(const std::filesystem::path& path)
    {
        std::vector<uint8_t> data;
        std::FILE* file = _wfopen(path.c_str(), L"rb");
        if (file == nullptr)
            return data;
        if (_fseeki64(file, 0, SEEK_END) == 0)
        {
            long long size = _ftelli64(file);
            if (size > 0 && _fseeki64(file, 0, SEEK_SET) == 0)
            {
                data.resize(size_t(size));
                if (std::fread(data.data(), 1, data.size(), file) != data.size())
                    data.clear();
            }
        }
        std::fclose(file);
        return data;
    }
}
