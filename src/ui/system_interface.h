#pragma once

// RmlUi's system and file interfaces: the host clock, the port's log, SDL's cursors and clipboard,
// and the files under data/ui. Host main thread only.

#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/SystemInterface.h>

#include <filesystem>
#include <unordered_map>

struct SDL_Cursor;

namespace ui
{
    class SystemInterface final : public Rml::SystemInterface
    {
    public:
        ~SystemInterface() override;

        double GetElapsedTime() override;
        // Warnings and errors always; the rest under ROAN_TRACE.
        bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;
        // CSS cursor names: pointer, text, move, wait, crosshair, not-allowed; anything else is the
        // arrow.
        void SetMouseCursor(const Rml::String& cursor_name) override;
        void SetClipboardText(const Rml::String& text) override;
        void GetClipboardText(Rml::String& text) override;
        // A game image's source ("game:<name>", render_interface.h) stays whole.
        void JoinPath(Rml::String& translated_path, const Rml::String& document_path, const Rml::String& path) override;

    private:
        std::unordered_map<int, SDL_Cursor*> cursors_; // by SDL_SystemCursor
        int current_ = -1;
    };

    // Opens files under one folder only: relative paths without "..".
    class FileInterface final : public Rml::FileInterface
    {
    public:
        explicit FileInterface(std::filesystem::path root) : root_(std::move(root)) {}

        Rml::FileHandle Open(const Rml::String& path) override;
        void Close(Rml::FileHandle file) override;
        size_t Read(void* buffer, size_t size, Rml::FileHandle file) override;
        bool Seek(Rml::FileHandle file, long offset, int origin) override;
        size_t Tell(Rml::FileHandle file) override;

    private:
        std::filesystem::path root_;
    };
}
