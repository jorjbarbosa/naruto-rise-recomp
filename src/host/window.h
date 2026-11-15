#pragma once

// The game window (SDL 3). Created and pumped on the host main thread.

namespace host
{
    // Initializes SDL video and opens the window. False on failure (logged).
    bool CreateMainWindow(const char* title, int width, int height);
    // HWND of the window, or null before it exists. Any thread.
    void* MainWindowHandle();
    // Pumps window events until the window is closed (returns true) or RequestQuit is called
    // (returns false).
    bool RunEventLoop();
    // Ends RunEventLoop. Any thread.
    void RequestQuit();
}
