#pragma once

// The game window (SDL 3). Created and pumped on the host main thread.

#include <cstdint>

struct SDL_Window;

namespace host
{
    // Initializes SDL (video, and gamepads and audio when available), applies the input settings and
    // opens the window with the stored mode, size, position and maximized state (settings.h). False
    // on failure (logged).
    bool CreateMainWindow(const char* title);
    // HWND of the window, or null before it exists. Any thread.
    void* MainWindowHandle();
    // The SDL window, or null before it exists. Host main thread.
    SDL_Window* MainWindow();
    // Refresh rate of the display the window is on, in millihertz (59.94 Hz is 59940); 0 when
    // unknown. Follows display and mode changes as the event loop sees them. Any thread.
    uint32_t DisplayRefreshMillihertz();
    // Pumps window events until the window is closed (returns true) or RequestQuit is called
    // (returns false). Alt+Enter switches between windowed and borderless; the windowed rectangle,
    // the maximized state and the mode go to the store as they change (saved by the caller). The
    // host UI (ui/ui.h) sees every other event before the game's input, and is woken as it asks.
    bool RunEventLoop();
    // Ends RunEventLoop. Any thread.
    void RequestQuit();
    // How many times F8 was pressed in the window: the runtime's diagnostic key, not a game input
    // (the state interpolation purity probe runs on it). Any thread.
    uint32_t DiagnosticKeyPresses();
}
