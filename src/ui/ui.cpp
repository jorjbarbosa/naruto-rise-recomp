// The host UI (ui.h): RmlUi's context, the open document, input routing and the wakes.

#include "ui/ui.h"

#include "game/input_hold.h"
#include "gpu/ui.h"
#include "host/input.h"
#include "host/paths.h"
#include "host/timing.h"
#include "log.h"
#include "ui/game_archive.h"
#include "ui/input.h"
#include "ui/render_interface.h"
#include "ui/system_interface.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <memory>

namespace ui
{
    namespace
    {
        // The stage the documents are laid out on, in dp.
        constexpr float kStageWidth = 1280.0f;
        constexpr float kStageHeight = 720.0f;
        // While no document is open, the cursor hides after this long without mouse motion.
        constexpr double kCursorIdleSeconds = 3.0;
        // While a document is open, the longest wait for an event, and the gap between presents that
        // is logged.
        constexpr int kOpenWaitMs = 100;
        // After a document closes the game's input stays held until every pad button (and the keys
        // bound to them) is released, so the press that closed it is not also the game's, but no
        // longer than this. Then the host's bit is cleared again for a while: the game sets its own
        // bits with plain read-modify-writes, which can put back a bit cleared in between.
        constexpr double kReleaseLimitSeconds = 3.0;
        constexpr double kClearSeconds = 0.5;
        constexpr int kReleasePollMs = 16;
        // A trigger past this counts as pressed.
        constexpr uint8_t kTriggerPressed = 30;

        struct State
        {
            SDL_Window* window = nullptr;
            GameArchive archive;
            std::unique_ptr<SystemInterface> system;
            std::unique_ptr<FileInterface> files;
            std::unique_ptr<RenderInterface> render;
            Rml::Context* context = nullptr;
            Rml::ElementDocument* document = nullptr;
            int width = 0; // the context's size, the window's in pixels
            int height = 0;
            PadActions pad;
            // The last input came from the mouse (hover drives, cursor shown) rather than from the
            // keyboard or a pad (focus drives, cursor hidden).
            bool mouseMode = true;
            // The element focused when the mouse took over, focused again when navigation returns.
            Rml::ObserverPtr<Rml::Element> lastFocus;
            bool cursorShown = true;
            int64_t cursorHideAt = 0; // no document open: when the idle cursor hides; 0 for never
            // A document closed: the game's input is released once nothing is pressed, or at
            // releaseBy; then the bit is cleared again until clearUntil (0: done).
            bool releasePending = false;
            int64_t releaseBy = 0;
            int64_t clearUntil = 0;
        };

        std::unique_ptr<State> g_state;
        std::atomic<bool> g_open{ false };
        uint32_t g_wakeEvent = 0;
        std::atomic<bool> g_wakePending{ false };
        std::atomic<int64_t> g_lastPresent{ 0 };

        // Queues one wake event unless one is pending. Any thread.
        void RequestWake()
        {
            if (!g_wakePending.exchange(true))
            {
                SDL_Event event{};
                event.type = g_wakeEvent;
                if (!SDL_PushEvent(&event))
                    g_wakePending = false;
            }
        }

        // The owner thread, after a present of a frame that shows something.
        void OnPresent()
        {
            if (!g_open.load(std::memory_order_acquire))
                return;
            int64_t now = host::Counter();
            int64_t last = g_lastPresent.exchange(now);
            if (last != 0 && now - last > host::CounterTicks(kOpenWaitMs / 1000.0))
                Log("ui: no present for %lld ms", (long long)((now - last) * 1000 / host::CounterFrequency()));
            RequestWake();
        }

        // Whether no pad button or trigger is pressed in any slot (the keyboard counts through the
        // keys bound to them).
        bool GameInputsUp()
        {
            for (int slot = 0; slot < host::kInputSlots; slot++)
            {
                host::PadState pad;
                if (host::GetPadState(slot, pad) &&
                    (pad.buttons != 0 || pad.leftTrigger > kTriggerPressed || pad.rightTrigger > kTriggerPressed))
                    return false;
            }
            return true;
        }

        void ShowCursor(State& state, bool shown)
        {
            if (shown == state.cursorShown)
                return;
            state.cursorShown = shown;
            if (shown)
                SDL_ShowCursor();
            else
                SDL_HideCursor();
        }

        // Follows the window's size in pixels.
        void ApplyWindowSize(State& state)
        {
            int width = 0, height = 0;
            if (!SDL_GetWindowSizeInPixels(state.window, &width, &height) || width <= 0 || height <= 0)
                return;
            if (width == state.width && height == state.height)
                return;
            state.width = width;
            state.height = height;
            state.context->SetDimensions(Rml::Vector2i(width, height));
            state.context->SetDensityIndependentPixelRatio(
                std::min(float(width) / kStageWidth, float(height) / kStageHeight));
        }

        bool Owned(const State& state, Rml::Element* element)
        {
            return element != nullptr && state.document != nullptr && element != state.document &&
                   element->GetOwnerDocument() == state.document;
        }

        void SetMouseMode(State& state, bool mouse)
        {
            if (mouse == state.mouseMode)
                return;
            state.mouseMode = mouse;
            if (state.document == nullptr)
                return;
            ShowCursor(state, mouse);
            if (mouse)
            {
                // Hover takes over: the focus goes, so the two never show on different elements.
                Rml::Element* focus = state.context->GetFocusElement();
                if (Owned(state, focus))
                {
                    state.lastFocus = focus->GetObserverPtr();
                    focus->Blur();
                }
            }
            else
                state.context->ProcessMouseLeave();
        }

        void PressKey(State& state, Rml::Input::KeyIdentifier key)
        {
            state.context->ProcessKeyDown(key, 0);
            state.context->ProcessKeyUp(key, 0);
        }

        void Execute(State& state, Action action)
        {
            if (state.document == nullptr)
                return;
            Rml::Element* focus = state.context->GetFocusElement();
            bool focused = Owned(state, focus);
            switch (action)
            {
            case Action::Up:
            case Action::Down:
            case Action::Left:
            case Action::Right:
                if (!focused)
                {
                    // Navigation starts where it left off, or on the first focusable element.
                    Rml::Element* start = Owned(state, state.lastFocus.get())
                                              ? state.lastFocus.get()
                                              : state.document->FindNextTabElement(state.document, true);
                    if (start != nullptr && start->Focus(true))
                        start->ScrollIntoView(Rml::ScrollAlignment::Adaptive);
                    break;
                }
                PressKey(state, action == Action::Up     ? Rml::Input::KI_UP
                                : action == Action::Down ? Rml::Input::KI_DOWN
                                : action == Action::Left ? Rml::Input::KI_LEFT
                                                         : Rml::Input::KI_RIGHT);
                break;
            case Action::Accept:
                if (focused)
                    PressKey(state, Rml::Input::KI_RETURN); // the document clicks the focused element
                break;
            case Action::Back:
            case Action::PreviousTab:
            case Action::NextTab:
            case Action::Reset:
            {
                const char* name = action == Action::Back          ? "back"
                                   : action == Action::PreviousTab ? "tabprevious"
                                   : action == Action::NextTab     ? "tabnext"
                                                                   : "reset";
                Rml::Element* target = focused ? focus : state.document;
                // A back that no element stops closes the document.
                if (target->DispatchEvent(name, Rml::Dictionary(), true, true) && action == Action::Back)
                    Close();
                break;
            }
            }
        }

        // No document open: releases the game's input after a close (State::releasePending).
        void FollowRelease(State& state, int64_t now)
        {
            if (state.releasePending)
            {
                if (now < state.releaseBy && !GameInputsUp())
                    return;
                state.releasePending = false;
                state.clearUntil = now + host::CounterTicks(kClearSeconds);
            }
            if (state.clearUntil != 0)
            {
                game::SetInputHeld(false);
                if (now >= state.clearUntil)
                    state.clearUntil = 0;
            }
        }

        void Execute(State& state, const std::vector<Action>& actions)
        {
            if (actions.empty() || state.document == nullptr)
                return;
            SetMouseMode(state, false);
            for (Action action : actions)
                Execute(state, action);
        }

        // SDL's window points to pixels, which the context is laid out in.
        Rml::Vector2f ToPixels(const State& state, float x, float y)
        {
            float density = SDL_GetWindowPixelDensity(state.window);
            if (density <= 0.0f)
                density = 1.0f;
            return Rml::Vector2f(x * density, y * density);
        }

        int MouseButton(uint8_t button)
        {
            switch (button)
            {
            case SDL_BUTTON_LEFT: return 0;
            case SDL_BUTTON_RIGHT: return 1;
            case SDL_BUTTON_MIDDLE: return 2;
            default: return -1;
            }
        }
    }

    bool Initialize(SDL_Window* window, const std::filesystem::path& uiDir, const std::filesystem::path& gameDir)
    {
        auto state = std::make_unique<State>();
        state->window = window;
        state->archive.Open(gameDir / "Naruto_clip.SRA"); // without it the documents have no game images
        state->system = std::make_unique<SystemInterface>();
        state->files = std::make_unique<FileInterface>(uiDir);
        state->render = std::make_unique<RenderInterface>(state->archive);
        Rml::SetSystemInterface(state->system.get());
        Rml::SetFileInterface(state->files.get());
        Rml::SetRenderInterface(state->render.get());
        if (!Rml::Initialise())
        {
            Log("ui: error: RmlUi does not start, no menus");
            return false;
        }

        int fonts = 0;
        std::error_code error;
        for (std::filesystem::directory_iterator it(uiDir / "fonts", error), end; !error && it != end;
             it.increment(error))
        {
            std::filesystem::path extension = it->path().extension();
            if (extension != ".ttf" && extension != ".otf")
                continue;
            std::u8string name = u8"fonts/" + it->path().filename().u8string();
            if (Rml::LoadFontFace(std::string(name.begin(), name.end())))
                fonts++;
        }
        g_wakeEvent = SDL_RegisterEvents(1);
        if (fonts > 0 && g_wakeEvent != 0)
            state->context = Rml::CreateContext("menu", Rml::Vector2i(1, 1));
        if (state->context == nullptr)
        {
            if (fonts == 0)
                Log("ui: error: no font in %s, no menus", host::PathText(uiDir / "fonts").c_str());
            else
                Log("ui: error: cannot create the UI's context or event, no menus: %s", SDL_GetError());
            Rml::Shutdown();
            return false;
        }
        ApplyWindowSize(*state);
        state->cursorHideAt = host::Counter() + host::CounterTicks(kCursorIdleSeconds);
        g_state = std::move(state);
        gpu::ui::SetPresentListener(OnPresent);
        Log("ui: ready, %d font face%s, %dx%d pixels", fonts, fonts == 1 ? "" : "s", g_state->width, g_state->height);
        return true;
    }

    void HandleEvent(const SDL_Event& event)
    {
        if (!g_state)
            return;
        State& state = *g_state;
        bool open = state.document != nullptr;
        int64_t now = host::Counter();
        std::vector<Action> actions;
        switch (event.type)
        {
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_WHEEL:
        {
            int modifiers = KeyModifiers(SDL_GetModState());
            if (!open)
            {
                state.mouseMode = true;
                ShowCursor(state, true);
                state.cursorHideAt = now + host::CounterTicks(kCursorIdleSeconds);
                break;
            }
            if (event.type == SDL_EVENT_MOUSE_WHEEL)
            {
                float direction = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
                // SDL: positive y is away from the user; RmlUi: positive is down.
                state.context->ProcessMouseWheel(Rml::Vector2f(event.wheel.x, -event.wheel.y) * direction, modifiers);
                break;
            }
            SetMouseMode(state, true);
            if (event.type == SDL_EVENT_MOUSE_MOTION)
            {
                Rml::Vector2f at = ToPixels(state, event.motion.x, event.motion.y);
                state.context->ProcessMouseMove(int(at.x), int(at.y), modifiers);
            }
            else if (int button = MouseButton(event.button.button); button >= 0)
            {
                Rml::Vector2f at = ToPixels(state, event.button.x, event.button.y);
                state.context->ProcessMouseMove(int(at.x), int(at.y), modifiers);
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                    state.context->ProcessMouseButtonDown(button, modifiers);
                else
                    state.context->ProcessMouseButtonUp(button, modifiers);
            }
            break;
        }
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            if (open)
                state.context->ProcessMouseLeave();
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        {
            if (!open)
            {
                if (event.type == SDL_EVENT_KEY_DOWN)
                    state.mouseMode = false;
                break;
            }
            Action action;
            if (KeyAction(event.key.scancode, action))
            {
                // The system's repeats repeat the directions only.
                if (event.type == SDL_EVENT_KEY_DOWN && (!event.key.repeat || IsDirection(action)))
                    actions.push_back(action);
                break;
            }
            Rml::Input::KeyIdentifier key = KeyIdentifier(event.key.scancode);
            if (key == Rml::Input::KI_UNKNOWN)
                break;
            int modifiers = KeyModifiers(event.key.mod);
            if (event.type == SDL_EVENT_KEY_DOWN)
            {
                if (key == Rml::Input::KI_TAB)
                    SetMouseMode(state, false);
                state.context->ProcessKeyDown(key, modifiers);
            }
            else
                state.context->ProcessKeyUp(key, modifiers);
            break;
        }
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        case SDL_EVENT_GAMEPAD_REMOVED:
            // Followed while closed too, so a stick held when a document opens is known.
            state.pad.HandleEvent(event, now, actions);
            if (!open)
            {
                if (!actions.empty())
                    state.mouseMode = false;
                actions.clear();
            }
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            state.pad.Reset();
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            ApplyWindowSize(state);
            break;
        default:
            break;
        }
        Execute(state, actions);
    }

    void Update()
    {
        g_wakePending = false;
        if (!g_state)
            return;
        State& state = *g_state;
        int64_t now = host::Counter();
        if (state.document == nullptr)
        {
            FollowRelease(state, now);
            if (state.cursorHideAt != 0 && now >= state.cursorHideAt)
            {
                ShowCursor(state, false);
                state.cursorHideAt = 0;
            }
            return;
        }
        // Again each time: the game zeroes its input flag once at start.
        game::SetInputHeld(true);
        std::vector<Action> actions;
        state.pad.Tick(now, actions);
        Execute(state, actions);
        if (state.document == nullptr)
            return;
        ApplyWindowSize(state);
        state.context->Update();
        state.render->BeginFrame(uint32_t(state.width), uint32_t(state.height));
        state.context->Render();
        gpu::ui::Submit(state.render->TakeFrame());
    }

    int WaitTimeoutMs()
    {
        if (!g_state)
            return -1;
        if (g_state->document != nullptr)
            return kOpenWaitMs;
        if (g_state->releasePending || g_state->clearUntil != 0)
            return kReleasePollMs;
        if (g_state->cursorHideAt == 0)
            return -1;
        int64_t left = g_state->cursorHideAt - host::Counter();
        return left <= 0 ? 0 : int(left * 1000 / host::CounterFrequency()) + 1;
    }

    bool IsWakeEvent(const SDL_Event& event)
    {
        return g_wakeEvent != 0 && event.type == g_wakeEvent;
    }

    bool Open(const char* name)
    {
        if (!g_state)
            return false;
        State& state = *g_state;
        Rml::ElementDocument* document = state.context->LoadDocument(name);
        if (document == nullptr)
        {
            Log("ui: error: cannot load the document %s", name);
            return false;
        }
        if (state.document != nullptr)
            state.document->Close();
        state.document = document;
        state.lastFocus.reset();
        document->Show();
        state.cursorHideAt = 0;
        ShowCursor(state, state.mouseMode);
        if (!state.mouseMode)
            state.context->ProcessMouseLeave();
        // A direction held in the game does not start moving the focus.
        state.pad.SuppressRepeat();
        state.releasePending = false;
        state.clearUntil = 0;
        game::SetInputHeld(true);
        g_lastPresent = 0;
        g_open.store(true, std::memory_order_release);
        // The first frame at the next turn of the loop (its presents then start the wakes), not
        // here: Open may be called from inside one of RmlUi's event handlers, where updating the
        // context is not allowed.
        RequestWake();
        return true;
    }

    void Close()
    {
        if (!g_state || g_state->document == nullptr)
            return;
        State& state = *g_state;
        g_open.store(false, std::memory_order_release);
        state.document->Close();
        state.document = nullptr;
        state.lastFocus.reset();
        state.pad.Reset();
        // RmlUi unloads the document at the next context update (the next Open's): closing may come
        // from inside one of its event handlers, where updating the context is not allowed.
        gpu::ui::Submit({});
        int64_t now = host::Counter();
        state.releasePending = true;
        state.releaseBy = now + host::CounterTicks(kReleaseLimitSeconds);
        if (state.cursorShown)
            state.cursorHideAt = now + host::CounterTicks(kCursorIdleSeconds);
    }

    bool IsOpen()
    {
        return g_open.load(std::memory_order_acquire);
    }
}
