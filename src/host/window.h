#pragma once

// The game window (SDL 3). Created and pumped on the host main thread.

#include <cstdint>

namespace host
{
    // Initializes SDL (video, and gamepads and audio when available) and opens the window. False
    // on failure (logged).
    bool CreateMainWindow(const char* title, int width, int height);
    // The window as the renderer takes it (plume::RenderWindow): its HWND on Windows, the SDL_Window
    // elsewhere; null before it exists. Any thread.
    void* MainWindowHandle();
    // Refresh rate of the display the window is on, in millihertz (59.94 Hz is 59940); 0 when
    // unknown. Follows display and mode changes as the event loop sees them. Any thread.
    uint32_t DisplayRefreshMillihertz();
    // Pumps window events until the window is closed (returns true) or RequestQuit is called
    // (returns false).
    bool RunEventLoop();
    // Ends RunEventLoop. Any thread.
    void RequestQuit();
    // How many times F8 was pressed in the window: the runtime's diagnostic key, not a game input
    // (the state interpolation purity probe runs on it). Any thread.
    uint32_t DiagnosticKeyPresses();
}
