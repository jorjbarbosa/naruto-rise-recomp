// RmlUi's system and file interfaces (system_interface.h).

#include "ui/system_interface.h"

#include "host/timing.h"
#include "log.h"
#include "ui/render_interface.h"

#include <SDL3/SDL.h>

#include <cstdio>

namespace ui
{
    SystemInterface::~SystemInterface()
    {
        for (auto& [id, cursor] : cursors_)
            SDL_DestroyCursor(cursor);
    }

    double SystemInterface::GetElapsedTime()
    {
        static const int64_t start = host::Counter();
        return double(host::Counter() - start) / double(host::CounterFrequency());
    }

    bool SystemInterface::LogMessage(Rml::Log::Type type, const Rml::String& message)
    {
        switch (type)
        {
        case Rml::Log::LT_ALWAYS:
        case Rml::Log::LT_ERROR:
        case Rml::Log::LT_ASSERT:
            Log("ui: error: %s", message.c_str());
            break;
        case Rml::Log::LT_WARNING:
            Log("ui: warning: %s", message.c_str());
            break;
        default:
            TRACE("ui: %s", message.c_str());
            break;
        }
        return true; // an assertion logs and goes on
    }

    void SystemInterface::SetMouseCursor(const Rml::String& name)
    {
        SDL_SystemCursor id = SDL_SYSTEM_CURSOR_DEFAULT;
        if (name == "pointer")
            id = SDL_SYSTEM_CURSOR_POINTER;
        else if (name == "text")
            id = SDL_SYSTEM_CURSOR_TEXT;
        else if (name == "move")
            id = SDL_SYSTEM_CURSOR_MOVE;
        else if (name == "wait")
            id = SDL_SYSTEM_CURSOR_WAIT;
        else if (name == "crosshair")
            id = SDL_SYSTEM_CURSOR_CROSSHAIR;
        else if (name == "not-allowed")
            id = SDL_SYSTEM_CURSOR_NOT_ALLOWED;
        if (int(id) == current_)
            return;
        SDL_Cursor*& cursor = cursors_[int(id)];
        if (cursor == nullptr)
            cursor = SDL_CreateSystemCursor(id);
        if (cursor != nullptr && SDL_SetCursor(cursor))
            current_ = int(id);
    }

    void SystemInterface::SetClipboardText(const Rml::String& text)
    {
        SDL_SetClipboardText(text.c_str());
    }

    void SystemInterface::GetClipboardText(Rml::String& text)
    {
        char* clipboard = SDL_GetClipboardText();
        text = clipboard != nullptr ? clipboard : "";
        SDL_free(clipboard);
    }

    void SystemInterface::JoinPath(Rml::String& translated, const Rml::String& document, const Rml::String& path)
    {
        if (path.rfind(RenderInterface::kGamePrefix, 0) == 0)
            translated = path;
        else
            Rml::SystemInterface::JoinPath(translated, document, path);
    }

    Rml::FileHandle FileInterface::Open(const Rml::String& path)
    {
        std::filesystem::path relative;
        try
        {
            relative = std::filesystem::path(std::u8string(path.begin(), path.end())).lexically_normal();
        }
        catch (const std::exception&)
        {
            relative.clear(); // not UTF-8
        }
        if (relative.empty() || relative.is_absolute() || relative.has_root_name() || *relative.begin() == "..")
        {
            Log("ui: error: %s is not a path under data/ui", path.c_str());
            return 0;
        }
        FILE* file = _wfopen((root_ / relative).c_str(), L"rb");
        return Rml::FileHandle(file);
    }

    void FileInterface::Close(Rml::FileHandle file)
    {
        fclose(reinterpret_cast<FILE*>(file));
    }

    size_t FileInterface::Read(void* buffer, size_t size, Rml::FileHandle file)
    {
        return fread(buffer, 1, size, reinterpret_cast<FILE*>(file));
    }

    bool FileInterface::Seek(Rml::FileHandle file, long offset, int origin)
    {
        return fseek(reinterpret_cast<FILE*>(file), offset, origin) == 0;
    }

    size_t FileInterface::Tell(Rml::FileHandle file)
    {
        return size_t(ftell(reinterpret_cast<FILE*>(file)));
    }
}
