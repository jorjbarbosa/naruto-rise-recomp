#pragma once

// The port's settings (port menus, phases 1 and 2): one TOML file in the user folder,
// data/user/settings.toml, with an environment variable as an override for one run on some of them.
//
// Every setting is one entry of the option table in settings.cpp: its TOML table and key, its
// variable, its kind, its default and its comment. The reader, the override, the writer and later the
// menu pages walk that table; callers only use the accessors below.
//
// Each setting has a stored value (the file's, what Save writes) and an effective one (the stored
// value, or the variable's when it is set: what the accessors return). Load runs on the main thread
// before any other thread exists. The window's values change afterwards through the setters, on the
// main thread, and are read there only; every other value is set once by Load and may be read from
// any thread. Before Load the accessors return the defaults.

#include <array>
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

    enum class WindowMode : uint8_t
    {
        Windowed,
        Borderless, // fullscreen on the window's monitor, without a display mode change
    };

    // The pad inputs the keyboard can drive, in the order of the [keyboard] table.
    enum class PadInput : uint8_t
    {
        A, B, X, Y, Start, Back, LeftShoulder, RightShoulder, LeftThumb, RightThumb,
        LeftTrigger, RightTrigger, DpadUp, DpadDown, DpadLeft, DpadRight,
        LeftStickUp, LeftStickDown, LeftStickLeft, LeftStickRight,
        RightStickUp, RightStickDown, RightStickLeft, RightStickRight,
        Count,
    };

    // The window's rectangle in windowed mode, in SDL window coordinates.
    struct WindowRect
    {
        int32_t x = 0, y = 0;
        int32_t width = 0, height = 0;
        bool centerX = false, centerY = false; // "center": centred on the primary monitor
    };

    // Reads the file, applies the environment, and writes the file back when it was missing,
    // invalid or lacked a key (a file of a newer version is never written). Logs the values that are
    // not at their defaults. Main thread, once, before the window, the renderer and the guest start.
    void Load(const std::filesystem::path& file);

    // Writes the stored values to the file given to Load, keeping the keys this build does not know.
    // Main thread. False when nothing was written (logged).
    bool Save();
    // Save, when a setter changed a value since the last write. Main thread.
    bool SaveIfChanged();

    // Where FrameRate finds the monitor's refresh rate for "display" (millihertz, 0: unknown). Main
    // thread, before the guest starts.
    void SetDisplayRefreshSource(uint32_t (*millihertz)());

    // video.vsync: wait for the vertical blank when presenting.
    bool VSync();
    // video.frame_rate: 0 for the game's own rate, else the state interpolation rate (31 to 240).
    // "display" gives the monitor's rate at the time of the call.
    uint32_t FrameRate();
    // video.adapter: the graphics adapter as named in the log; empty for the default one.
    const std::string& Adapter();

    // window.*: main thread only.
    WindowMode GetWindowMode();
    WindowRect GetWindowRect();
    bool WindowMaximized();
    void SetWindowMode(WindowMode mode);
    void SetWindowRect(int32_t x, int32_t y, int32_t width, int32_t height);
    void SetWindowMaximized(bool maximized);

    // audio.volume: 0 to 100.
    uint32_t Volume();
    // audio.device: the playback device as named in the log; empty for the system's default.
    const std::string& AudioDevice();
    // audio.mute_in_background.
    bool MuteInBackground();

    // gamepad.dead_zone: percent of full deflection, 0 to 50.
    uint32_t DeadZone();
    // keyboard.*: the SDL scancodes of a pad input, 0 for an empty slot.
    std::array<uint16_t, 2> KeyboardKeys(PadInput input);

    // game.language.
    Language GameLanguage();
    // The XDK language of game.language: the answer to ExGetXConfigSetting(3, 9).
    uint32_t XdkLanguage();
}
