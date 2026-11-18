#pragma once

// The port's settings (port menus, phase 1): one TOML file in the user folder,
// data/user/settings.toml, with an environment variable per setting as an override for one run.
//
// Every setting is one entry of the option table in settings.cpp: its TOML table and key, its
// variable, its kind, its default and its comment. The reader, the override, the writer and later the
// menu pages walk that table; callers only use the accessors below.
//
// Each setting has a stored value (the file's, what Save writes) and an effective one (the stored
// value, or the variable's when it is set: what the accessors return). Load runs on the main thread
// before any other thread exists and the values do not change afterwards, so the accessors may be
// called from any thread. Before Load they return the defaults.

#include <cstdint>
#include <filesystem>
#include <string>

namespace settings
{
    enum class Language : uint8_t
    {
        English,
        French,
        Spanish,
        Italian,
        German,
    };

    // Reads the file, applies the environment, and writes the file back when it was missing,
    // invalid or lacked a key (a file of a newer version is never written). Logs the values. Main
    // thread, once, before the window, the renderer and the guest start.
    void Load(const std::filesystem::path& file);

    // Writes the stored values to the file given to Load, keeping the keys this build does not know.
    // Main thread. False when nothing was written (logged).
    bool Save();

    // video.vsync: wait for the vertical blank when presenting.
    bool VSync();
    // video.frame_rate: 0 for the game's own rate, else the state interpolation rate (31 to 240).
    uint32_t FrameRate();
    // video.adapter: the graphics adapter as named in the log; empty for the default one.
    const std::string& Adapter();
    // game.language.
    Language GameLanguage();
    // The XDK language of game.language: the answer to ExGetXConfigSetting(3, 9).
    uint32_t XdkLanguage();
}
