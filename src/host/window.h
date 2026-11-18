#pragma once

// The game window (SDL 3). Created and pumped on the host main thread.

#include <cstdint>
#include <string>
#include <vector>

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

    // ---- For the menu pages: host main thread. The store follows as with Alt+Enter and the
    // window's own moves (window.* in settings.h). ----

    bool IsBorderless();
    // Switches between windowed and borderless, as Alt+Enter.
    void SetBorderless(bool borderless);

    struct WindowSize
    {
        int width = 0;
        int height = 0;
        bool operator==(const WindowSize&) const = default;
    };
    // The window's size in windowed mode (its client area, in SDL window coordinates).
    WindowSize CurrentWindowSize();
    // The usual sizes of a 16:9 window that fit the usable area of the window's display, smallest
    // first, with the current size among them.
    std::vector<WindowSize> WindowSizes();
    // Resizes the window in windowed mode (restoring it first if it is maximized), keeping its centre.
    void SetWindowSize(WindowSize size);

    // The connected displays, by name, in SDL's order, and the one the window is on (-1 if unknown).
    std::vector<std::string> DisplayNames();
    int WindowDisplay();
    // Moves the window to a display: centred in windowed mode, filling it when borderless.
    void MoveToDisplay(int index);
}
