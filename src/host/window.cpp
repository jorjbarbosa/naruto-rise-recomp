// The SDL window, its event loop and the refresh rate of the display it is on.

#include "host/window.h"

#include "host/audio.h"
#include "host/input.h"
#include "log.h"
#include "settings/settings.h"
#include "ui/ui.h"

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
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
        // The window is borderless, or is becoming so: its moves and resizes are not the windowed
        // rectangle. Set before SDL is asked, so the events of the switch are already covered. Main
        // thread.
        bool g_borderless = false;

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
        std::atomic<uint32_t> g_diagnosticKeyPresses{ 0 };
        constexpr SDL_Scancode kDiagnosticKey = SDL_SCANCODE_F8;

        // The display whose usable area holds the point, or 0.
        SDL_DisplayID DisplayAt(int x, int y)
        {
            int count = 0;
            SDL_DisplayID* displays = SDL_GetDisplays(&count);
            SDL_DisplayID found = 0;
            SDL_Point point{ x, y };
            for (int i = 0; i < count && found == 0; i++)
            {
                SDL_Rect bounds;
                if (SDL_GetDisplayUsableBounds(displays[i], &bounds) && SDL_PointInRect(&point, &bounds))
                    found = displays[i];
            }
            SDL_free(displays);
            return found;
        }

        // The stored windowed rectangle, placed: a position whose centre is on no connected display
        // is centred on the primary one (relocated is set); the size is cut to the usable area of its
        // display.
        SDL_Rect PlaceWindow(bool& relocated)
        {
            relocated = false;
            settings::WindowRect stored = settings::GetWindowRect();
            SDL_Rect rect{ stored.x, stored.y, stored.width, stored.height };
            bool centered = stored.centerX || stored.centerY;
            SDL_DisplayID display = 0;
            if (!centered)
            {
                display = DisplayAt(rect.x + rect.w / 2, rect.y + rect.h / 2);
                if (display == 0)
                {
                    Log("window: the saved position %d,%d is on no monitor, centring the window", rect.x, rect.y);
                    centered = true;
                    relocated = true;
                }
            }
            if (display == 0)
                display = SDL_GetPrimaryDisplay();
            SDL_Rect bounds;
            if (display != 0 && SDL_GetDisplayUsableBounds(display, &bounds))
            {
                rect.w = std::min(rect.w, bounds.w);
                rect.h = std::min(rect.h, bounds.h);
            }
            if (centered)
            {
                rect.x = int(SDL_WINDOWPOS_CENTERED_DISPLAY(display));
                rect.y = int(SDL_WINDOWPOS_CENTERED_DISPLAY(display));
            }
            return rect;
        }

        void SwitchBorderless(bool borderless)
        {
            bool was = g_borderless;
            g_borderless = borderless;
            if (!SDL_SetWindowFullscreen(g_window, borderless))
            {
                Log("window: error: cannot switch to %s: %s", borderless ? "borderless" : "windowed", SDL_GetError());
                g_borderless = was;
                return;
            }
            settings::SetWindowMode(borderless ? settings::WindowMode::Borderless : settings::WindowMode::Windowed);
            Log("window: %s", borderless ? "borderless" : "windowed");
        }

        // Keeps the windowed rectangle and the maximized state in the store. Main thread.
        void HandleWindowEvent(const SDL_Event& event)
        {
            switch (event.type)
            {
            case SDL_EVENT_WINDOW_MOVED:
            case SDL_EVENT_WINDOW_RESIZED:
            {
                SDL_WindowFlags flags = SDL_GetWindowFlags(g_window);
                if (g_borderless || (flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED | SDL_WINDOW_FULLSCREEN)) != 0)
                    break;
                int x = 0, y = 0, width = 0, height = 0;
                if (SDL_GetWindowPosition(g_window, &x, &y) && SDL_GetWindowSize(g_window, &width, &height))
                    settings::SetWindowRect(x, y, width, height);
                break;
            }
            case SDL_EVENT_WINDOW_MAXIMIZED:
                if (!g_borderless)
                    settings::SetWindowMaximized(true);
                break;
            case SDL_EVENT_WINDOW_RESTORED:
                if (!g_borderless && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_MINIMIZED) == 0)
                    settings::SetWindowMaximized((SDL_GetWindowFlags(g_window) & SDL_WINDOW_MAXIMIZED) != 0);
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                SetAudioFocus(true);
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                SetAudioFocus(false);
                break;
            default:
                break;
            }
        }
    }

    bool CreateMainWindow(const char* title)
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
        ApplyInputSettings();

        bool relocated = false;
        SDL_Rect rect = PlaceWindow(relocated);
        g_borderless = settings::GetWindowMode() == settings::WindowMode::Borderless;
        SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
        if (settings::WindowMaximized())
            flags |= SDL_WINDOW_MAXIMIZED;
        if (g_borderless)
            flags |= SDL_WINDOW_FULLSCREEN; // no fullscreen mode set: SDL's desktop fullscreen
        SDL_PropertiesID properties = SDL_CreateProperties();
        SDL_SetStringProperty(properties, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
        SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_X_NUMBER, rect.x);
        SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_Y_NUMBER, rect.y);
        SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, rect.w);
        SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, rect.h);
        SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, flags);
        g_window = SDL_CreateWindowWithProperties(properties);
        SDL_DestroyProperties(properties);
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
        // A relocated window keeps its new place, so the next start does not relocate it again.
        if (relocated && !g_borderless && !settings::WindowMaximized())
        {
            int x = 0, y = 0, width = 0, height = 0;
            if (SDL_GetWindowPosition(g_window, &x, &y) && SDL_GetWindowSize(g_window, &width, &height))
                settings::SetWindowRect(x, y, width, height);
        }
        Log("window: %dx%d%s%s", rect.w, rect.h, settings::WindowMaximized() ? ", maximized" : "",
            g_borderless ? ", borderless" : "");
        UpdateRefreshRate();
        return true;
    }

    void* MainWindowHandle()
    {
        return g_handle;
    }

    SDL_Window* MainWindow()
    {
        return g_window;
    }

    uint32_t DisplayRefreshMillihertz()
    {
        return g_refreshMillihertz;
    }

    bool RunEventLoop()
    {
        SDL_Event event;
        for (;;)
        {
            int timeout = ui::WaitTimeoutMs();
            if (timeout < 0 ? !SDL_WaitEvent(&event) : !SDL_WaitEventTimeout(&event, timeout))
            {
                if (timeout < 0)
                    break;
                ui::Update(); // the wait ran out
                continue;
            }
            if (ui::IsWakeEvent(event))
            {
                ui::Update();
                continue;
            }
            if (event.type == SDL_EVENT_QUIT)
                return !g_quitRequested;
            if (event.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED || event.type == SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED)
                UpdateRefreshRate();
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == kDiagnosticKey && !event.key.repeat)
                g_diagnosticKeyPresses.fetch_add(1, std::memory_order_relaxed);
            // Alt+Enter switches the window mode and is not also a game input (Return is Start).
            if (event.type == SDL_EVENT_KEY_DOWN && (event.key.mod & SDL_KMOD_ALT) != 0 &&
                (event.key.scancode == SDL_SCANCODE_RETURN || event.key.scancode == SDL_SCANCODE_KP_ENTER))
            {
                if (!event.key.repeat)
                    SwitchBorderless(!g_borderless);
                continue;
            }
            ui::HandleEvent(event);
            HandleWindowEvent(event);
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

    uint32_t DiagnosticKeyPresses()
    {
        return g_diagnosticKeyPresses.load(std::memory_order_relaxed);
    }

    bool IsBorderless()
    {
        return g_borderless;
    }

    void SetBorderless(bool borderless)
    {
        if (borderless != g_borderless)
            SwitchBorderless(borderless);
    }

    WindowSize CurrentWindowSize()
    {
        settings::WindowRect rect = settings::GetWindowRect();
        return { rect.width, rect.height };
    }

    std::vector<WindowSize> WindowSizes()
    {
        static constexpr WindowSize kSizes[] = { { 1280, 720 }, { 1366, 768 }, { 1600, 900 }, { 1920, 1080 },
            { 2560, 1440 }, { 3200, 1800 }, { 3840, 2160 }, { 5120, 2880 }, { 7680, 4320 } };
        SDL_Rect bounds{ 0, 0, 0, 0 };
        SDL_DisplayID display = SDL_GetDisplayForWindow(g_window);
        if (display == 0 || !SDL_GetDisplayUsableBounds(display, &bounds))
            bounds = { 0, 0, 1 << 16, 1 << 16 };
        // The frame takes some of the usable area: a size whose client area fills it would not fit.
        int top = 0, left = 0, bottom = 0, right = 0;
        SDL_GetWindowBordersSize(g_window, &top, &left, &bottom, &right);
        std::vector<WindowSize> sizes;
        for (const WindowSize& size : kSizes)
            if (size.width + left + right <= bounds.w && size.height + top + bottom <= bounds.h)
                sizes.push_back(size);
        WindowSize current = CurrentWindowSize();
        if (std::find(sizes.begin(), sizes.end(), current) == sizes.end())
        {
            auto at = std::find_if(sizes.begin(), sizes.end(), [&](const WindowSize& size) {
                return int64_t(size.width) * size.height > int64_t(current.width) * current.height;
            });
            sizes.insert(at, current);
        }
        return sizes;
    }

    void SetWindowSize(WindowSize size)
    {
        if (g_borderless)
            return;
        if ((SDL_GetWindowFlags(g_window) & SDL_WINDOW_MAXIMIZED) != 0)
        {
            // Restoring is asynchronous: the centre is taken from the restored rectangle.
            SDL_RestoreWindow(g_window);
            SDL_SyncWindow(g_window);
        }
        int x = 0, y = 0, width = 0, height = 0;
        SDL_GetWindowPosition(g_window, &x, &y);
        SDL_GetWindowSize(g_window, &width, &height);
        SDL_SetWindowSize(g_window, size.width, size.height);
        SDL_SetWindowPosition(g_window, x + (width - size.width) / 2, y + (height - size.height) / 2);
        Log("window: %dx%d", size.width, size.height);
    }

    std::vector<std::string> DisplayNames()
    {
        std::vector<std::string> names;
        int count = 0;
        SDL_DisplayID* displays = SDL_GetDisplays(&count);
        for (int i = 0; i < count; i++)
        {
            const char* name = SDL_GetDisplayName(displays[i]);
            names.push_back(name != nullptr && *name != 0 ? name : "Display " + std::to_string(i + 1));
        }
        SDL_free(displays);
        return names;
    }

    int WindowDisplay()
    {
        SDL_DisplayID current = SDL_GetDisplayForWindow(g_window);
        int count = 0;
        SDL_DisplayID* displays = SDL_GetDisplays(&count);
        int found = -1;
        for (int i = 0; i < count && found < 0; i++)
            if (displays[i] == current)
                found = i;
        SDL_free(displays);
        return found;
    }

    void MoveToDisplay(int index)
    {
        int count = 0;
        SDL_DisplayID* displays = SDL_GetDisplays(&count);
        SDL_DisplayID display = index >= 0 && index < count ? displays[index] : 0;
        SDL_free(displays);
        if (display == 0 || display == SDL_GetDisplayForWindow(g_window))
            return;
        bool borderless = g_borderless;
        // A borderless window goes windowed for the move, then fills the new display.
        if (borderless)
            SwitchBorderless(false);
        if ((SDL_GetWindowFlags(g_window) & SDL_WINDOW_MAXIMIZED) != 0)
            SDL_RestoreWindow(g_window);
        SDL_SetWindowPosition(g_window, int(SDL_WINDOWPOS_CENTERED_DISPLAY(display)),
            int(SDL_WINDOWPOS_CENTERED_DISPLAY(display)));
        SDL_SyncWindow(g_window);
        if (borderless)
            SwitchBorderless(true);
        Log("window: moved to display %d", index + 1);
    }
}
