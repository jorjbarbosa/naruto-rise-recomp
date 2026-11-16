// The SDL window, its event loop and the refresh rate of the display it is on.

#include "host/window.h"

#include "host/input.h"
#include "log.h"

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <atomic>
#include <cmath>

namespace host
{
    namespace
    {
        SDL_Window* g_window = nullptr;
        std::atomic<void*> g_handle{ nullptr };
        std::atomic<bool> g_quitRequested{ false };
        std::atomic<uint32_t> g_refreshMillihertz{ 0 };

        // Reads the current mode of the window's display. Main thread.
        void UpdateRefreshRate()
        {
            const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_window));
            uint32_t millihertz = 0;
            if (mode != nullptr && mode->refresh_rate_numerator > 0 && mode->refresh_rate_denominator > 0)
                millihertz = uint32_t(int64_t(mode->refresh_rate_numerator) * 1000 / mode->refresh_rate_denominator);
            else if (mode != nullptr && mode->refresh_rate > 0.0f)
                millihertz = uint32_t(std::lround(mode->refresh_rate * 1000.0f));
            g_refreshMillihertz = millihertz;
        }
    }

    bool CreateMainWindow(const char* title, int width, int height)
    {
        SDL_SetMainReady();
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            Log("error: SDL_Init failed: %s", SDL_GetError());
            return false;
        }
        // Without gamepads the game still runs on the keyboard.
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
            Log("error: no gamepad support, keyboard only: %s", SDL_GetError());
        // Without audio the game runs silent (host/audio.cpp keeps time instead).
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
            Log("error: no audio support: %s", SDL_GetError());
        g_window = SDL_CreateWindow(title, width, height, SDL_WINDOW_RESIZABLE);
        if (g_window == nullptr)
        {
            Log("error: SDL_CreateWindow failed: %s", SDL_GetError());
            return false;
        }
        g_handle = SDL_GetPointerProperty(SDL_GetWindowProperties(g_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        if (g_handle == nullptr)
        {
            Log("error: the window has no HWND: %s", SDL_GetError());
            return false;
        }
        UpdateRefreshRate();
        return true;
    }

    void* MainWindowHandle()
    {
        return g_handle;
    }

    uint32_t DisplayRefreshMillihertz()
    {
        return g_refreshMillihertz;
    }

    bool RunEventLoop()
    {
        SDL_Event event;
        while (SDL_WaitEvent(&event))
        {
            if (event.type == SDL_EVENT_QUIT)
                return !g_quitRequested;
            if (event.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED || event.type == SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED)
                UpdateRefreshRate();
            HandleInputEvent(event);
        }
        Log("error: SDL_WaitEvent failed: %s", SDL_GetError());
        return false;
    }

    void RequestQuit()
    {
        g_quitRequested = true;
        SDL_Event event{};
        event.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&event);
    }
}
