// The host UI (ui.h): RmlUi's context, the stack of open documents, input routing and the wakes.

#include "ui/ui.h"

#include "game/input_hold.h"
#include "gpu/ui.h"
#include "host/input.h"
#include "host/paths.h"
#include "host/quit.h"
#include "host/timing.h"
#include "log.h"
#include "ui/font_effects.h"
#include "ui/game_archive.h"
#include "ui/input.h"
#include "ui/pages.h"
#include "ui/render_interface.h"
#include "ui/system_interface.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

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
        // The poll of the release above and of OpenWhen's condition.
        constexpr int kPollMs = 16;
        // A trigger past this counts as pressed.
        constexpr uint8_t kTriggerPressed = 30;
        // The key's menu blurs the game image behind it in this long.
        constexpr double kBlurFadeSeconds = 0.25;

        // An open document and the element focused below it when it was pushed.
        struct Layer
        {
            Rml::ElementDocument* document = nullptr;
            Rml::ObserverPtr<Rml::Element> focusBelow;
        };

        // The last frame kept after a close (HoldLastFrame).
        struct Hold
        {
            bool (*done)() = nullptr;
            int64_t deadline = 0;
        };

        // A root waiting for a condition (OpenWhen).
        struct PendingOpen
        {
            std::string document;
            Entry entry = Entry::Key;
            bool (*ready)() = nullptr;
            void (*expired)() = nullptr;
            int64_t deadline = 0;
        };

        // Runs the data-action of a clicked element (ui.h).
        class ActionListener final : public Rml::EventListener
        {
        public:
            void ProcessEvent(Rml::Event& event) override;
        };

        struct State
        {
            SDL_Window* window = nullptr;
            GameArchive archive;
            std::unique_ptr<SystemInterface> system;
            std::unique_ptr<FileInterface> files;
            std::unique_ptr<RenderInterface> render;
            Rml::Context* context = nullptr;
            std::vector<Layer> stack; // the root first
            Entry entry = Entry::Options;
            int64_t openedAt = 0;
            Handlers handlers;
            std::unique_ptr<PendingOpen> pending;
            Hold hold; // done is null when none
            ActionListener actions;
            int width = 0; // the context's size, the window's in pixels
            int height = 0;
            PadActions pad;
            // The last input came from the mouse (hover drives, cursor shown) rather than from the
            // keyboard or a pad (focus drives, cursor hidden).
            bool mouseMode = true;
            // The device last used, as the documents' input attribute says it.
            const char* device = "mouse";
            // CaptureKey's handler: the next key down goes to it.
            void (*capture)(int scancode) = nullptr;
            // The element navigation starts from: the one focused when the mouse took over, or the
            // one a document opened on.
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
        std::atomic<bool> g_available{ false };
        std::atomic<bool> g_open{ false };
        uint32_t g_wakeEvent = 0;
        std::atomic<bool> g_wakePending{ false };
        std::atomic<int64_t> g_lastPresent{ 0 };

        // RequestOpen's requests, taken by Update.
        std::mutex g_requestMutex;
        std::vector<std::pair<std::string, Entry>> g_requests;

        const char* EntryName(Entry entry)
        {
            switch (entry)
            {
            case Entry::Options: return "options";
            case Entry::Key: return "key";
            case Entry::MainMenuQuit: return "main-menu-quit";
            }
            return "";
        }

        Rml::ElementDocument* Top(const State& state)
        {
            return state.stack.empty() ? nullptr : state.stack.back().document;
        }

        // Marks every open document with the device last used.
        void SetDevice(State& state, const char* device)
        {
            if (device == state.device)
                return;
            state.device = device;
            for (Layer& layer : state.stack)
                layer.document->SetAttribute("input", device);
        }

        // Queues one wake event unless one is pending. Any thread.
        void RequestWake()
        {
            if (g_wakeEvent != 0 && !g_wakePending.exchange(true))
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
            float ratio = std::min(float(width) / kStageWidth, float(height) / kStageHeight);
            state.context->SetDensityIndependentPixelRatio(ratio);
            // The font effects' lengths are fixed when a style sheet is parsed (font_effects.h).
            if (font_effects::SetDensityRatio(ratio))
            {
                Rml::Factory::ClearStyleSheetCache();
                for (const Layer& layer : state.stack)
                    layer.document->ReloadStyleSheet();
            }
        }

        // Whether an element belongs to the top document (the only one input reaches).
        bool Owned(const State& state, Rml::Element* element)
        {
            Rml::ElementDocument* top = Top(state);
            return element != nullptr && top != nullptr && element != top && element->GetOwnerDocument() == top;
        }

        void SetMouseMode(State& state, bool mouse)
        {
            if (mouse == state.mouseMode)
                return;
            state.mouseMode = mouse;
            if (state.stack.empty())
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

        // Loads a document with the entry's attribute and the action listener; null when it cannot.
        Rml::ElementDocument* Load(State& state, const char* name)
        {
            Rml::ElementDocument* document = state.context->LoadDocument(name);
            if (document == nullptr)
            {
                Log("ui: error: cannot load the document %s", name);
                return nullptr;
            }
            document->SetAttribute("entry", EntryName(state.entry));
            document->SetAttribute("input", state.device);
            document->AddEventListener(Rml::EventId::Click, &state.actions);
            pages::Attach(document);
            return document;
        }

        // Shows a loaded document on top of the stack, the focus on its element with the autofocus
        // attribute or else its first focusable one.
        void ShowOnTop(State& state, Rml::ElementDocument* document, bool modal)
        {
            Layer layer;
            layer.document = document;
            if (Rml::Element* focus = state.context->GetFocusElement(); Owned(state, focus))
                layer.focusBelow = focus->GetObserverPtr();
            else
                layer.focusBelow = state.lastFocus;
            state.stack.push_back(layer);
            document->Show(modal ? Rml::ModalFlag::Modal : Rml::ModalFlag::None, Rml::FocusFlag::Document);
            Rml::Element* start = document->QuerySelector("[autofocus]");
            if (start == nullptr)
                start = document->FindNextTabElement(document, true);
            state.lastFocus = start != nullptr ? start->GetObserverPtr() : Rml::ObserverPtr<Rml::Element>();
            if (!state.mouseMode && start != nullptr)
                start->Focus(true);
        }

        void Pop(State& state);

        void RunAction(State& state, const Rml::String& action)
        {
            if (action == "back")
                Pop(state);
            else if (action == "quit")
                host::Quit("the menu's quit");
            else if (action.rfind("push:", 0) == 0)
                Push(action.substr(5).c_str());
            else
                Log("ui: unknown action \"%s\"", action.c_str());
        }

        void ActionListener::ProcessEvent(Rml::Event& event)
        {
            if (!g_state)
                return;
            for (Rml::Element* element = event.GetTargetElement(); element != nullptr;
                 element = element->GetParentNode())
            {
                if (const Rml::Variant* action = element->GetAttribute("data-action"))
                {
                    RunAction(*g_state, action->Get<Rml::String>());
                    return;
                }
                if (element == element->GetOwnerDocument())
                    return;
            }
        }

        // Closes the top document; the root closes everything.
        void Pop(State& state)
        {
            if (state.stack.size() <= 1)
            {
                Close();
                return;
            }
            Layer layer = state.stack.back();
            state.stack.pop_back();
            state.capture = nullptr;
            // RmlUi unloads it at the next context update: this may be inside one of its handlers.
            layer.document->Close();
            state.lastFocus = layer.focusBelow;
            Rml::Element* restore = layer.focusBelow.get();
            if (!state.mouseMode && Owned(state, restore))
                restore->Focus(true);
            else
                Top(state)->Focus();
        }

        void Execute(State& state, Action action)
        {
            if (state.stack.empty())
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
                    Rml::ElementDocument* top = Top(state);
                    Rml::Element* start = Owned(state, state.lastFocus.get()) ? state.lastFocus.get()
                                                                              : top->FindNextTabElement(top, true);
                    if (start != nullptr && start->Focus(true))
                        start->ScrollIntoView(Rml::ScrollAlignment::Adaptive);
                    break;
                }
                // Left and right change the focused element's value when it takes them.
                if (action == Action::Left || action == Action::Right)
                {
                    Rml::Dictionary parameters;
                    parameters["delta"] = action == Action::Left ? -1 : 1;
                    if (!focus->DispatchEvent("step", parameters, true, true))
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
                else if (Rml::Element* accept = Top(state)->QuerySelector("[data-accept]"))
                    accept->Click();
                break;
            case Action::Menu:
                // The key that opened the key's menu closes it from anywhere; elsewhere it is Back.
                if (state.entry == Entry::Key)
                {
                    Close();
                    break;
                }
                [[fallthrough]];
            case Action::Back:
            case Action::PreviousTab:
            case Action::NextTab:
            case Action::Reset:
            {
                bool back = action == Action::Back || action == Action::Menu;
                const char* name = back                             ? "back"
                                   : action == Action::PreviousTab ? "tabprevious"
                                   : action == Action::NextTab     ? "tabnext"
                                                                   : "reset";
                Rml::Element* target = focused ? focus : Top(state);
                // A back that no element stops pops the document.
                if (target->DispatchEvent(name, Rml::Dictionary(), true, true) && back)
                    Pop(state);
                break;
            }
            }
        }

        // Starts releasing the game's input (State::releasePending) and the idle cursor's timer.
        void StartRelease(State& state, int64_t now)
        {
            state.releasePending = true;
            state.releaseBy = now + host::CounterTicks(kReleaseLimitSeconds);
            if (state.cursorShown)
                state.cursorHideAt = now + host::CounterTicks(kCursorIdleSeconds);
        }

        // No document open: drops the held frame once it is done with, then releases the game's input
        // after a close (State::releasePending).
        void FollowRelease(State& state, int64_t now)
        {
            if (state.hold.done != nullptr)
            {
                bool done = state.hold.done();
                if (!done && now < state.hold.deadline)
                    return;
                if (!done)
                    Log("ui: warning: the game did not take its screen down in time");
                state.hold = {};
                gpu::ui::Submit({});
                StartRelease(state, now);
                return;
            }
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
            if (actions.empty() || state.stack.empty())
                return;
            SetMouseMode(state, false);
            for (Action action : actions)
            {
                Execute(state, action);
                if (state.stack.empty())
                    break;
            }
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

        bool RequestsPending()
        {
            std::lock_guard lock(g_requestMutex);
            return !g_requests.empty();
        }

        // The menu key with nothing open or about to open goes to the handler.
        void MenuKey(State& state)
        {
            if (state.stack.empty() && !state.pending && state.hold.done == nullptr && !RequestsPending() &&
                state.handlers.menuKey != nullptr)
                state.handlers.menuKey();
        }

        void Refuse(State& state, const std::string& document, Entry entry, const char* why)
        {
            Log("ui: %s not opened (%s): %s", document.c_str(), EntryName(entry), why);
            if (state.handlers.closed != nullptr)
                state.handlers.closed(entry);
        }

        // Opens RequestOpen's requests and OpenWhen's root when it is due.
        void OpenPending(State& state, int64_t now)
        {
            std::vector<std::pair<std::string, Entry>> requests;
            {
                std::lock_guard lock(g_requestMutex);
                requests.swap(g_requests);
            }
            for (auto& [document, entry] : requests)
            {
                if (!state.stack.empty() || state.pending || state.hold.done != nullptr)
                    Refuse(state, document, entry, "another menu is open or closing");
                else
                    Open(document.c_str(), entry);
            }
            if (state.pending && state.stack.empty())
            {
                std::unique_ptr<PendingOpen> pending;
                if (state.pending->ready())
                {
                    pending = std::move(state.pending);
                    Open(pending->document.c_str(), pending->entry);
                }
                else if (now >= state.pending->deadline)
                {
                    pending = std::move(state.pending);
                    pending->expired();
                }
            }
        }
    }

    bool Initialize(SDL_Window* window, const std::filesystem::path& uiDir, const vfs::Source& game)
    {
        auto state = std::make_unique<State>();
        state->window = window;
        state->archive.Open(game, "Naruto_clip.SRA"); // without it the documents have no game images
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
        font_effects::Register();

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
        g_available.store(true, std::memory_order_release);
        Log("ui: ready, %d font face%s, %dx%d pixels", fonts, fonts == 1 ? "" : "s", g_state->width, g_state->height);
        return true;
    }

    bool IsAvailable()
    {
        return g_available.load(std::memory_order_acquire);
    }

    void SetHandlers(const Handlers& handlers)
    {
        if (g_state)
            g_state->handlers = handlers;
    }

    void HandleEvent(const SDL_Event& event)
    {
        if (!g_state)
            return;
        State& state = *g_state;
        bool open = !state.stack.empty();
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
            SetDevice(state, "mouse");
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
            if (event.type == SDL_EVENT_KEY_DOWN)
                SetDevice(state, "keyboard");
            if (!open)
            {
                if (event.type == SDL_EVENT_KEY_DOWN)
                {
                    state.mouseMode = false;
                    if (event.key.scancode == SDL_SCANCODE_ESCAPE && !event.key.repeat)
                        MenuKey(state);
                }
                break;
            }
            if (state.capture != nullptr)
            {
                if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat)
                {
                    auto handler = state.capture;
                    state.capture = nullptr;
                    SetMouseMode(state, false);
                    handler(int(event.key.scancode));
                }
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
            if (!actions.empty())
                SetDevice(state, "pad");
            if (!open)
            {
                if (!actions.empty())
                    state.mouseMode = false;
                if (std::find(actions.begin(), actions.end(), Action::Menu) != actions.end())
                    MenuKey(state);
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
        OpenPending(state, now);
        if (state.stack.empty())
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
        pages::Update();
        std::vector<Action> actions;
        state.pad.Tick(now, actions);
        Execute(state, actions);
        if (state.stack.empty())
            return;
        ApplyWindowSize(state);
        state.context->Update();
        state.render->BeginFrame(uint32_t(state.width), uint32_t(state.height));
        state.context->Render();
        gpu::ui::Frame frame = state.render->TakeFrame();
        if (state.entry == Entry::Key)
            frame.blur = float(std::min(1.0, double(now - state.openedAt) / double(host::CounterTicks(kBlurFadeSeconds))));
        gpu::ui::Submit(std::move(frame));
    }

    int WaitTimeoutMs()
    {
        if (!g_state)
            return -1;
        if (!g_state->stack.empty())
            return kOpenWaitMs;
        if (g_state->pending || g_state->hold.done != nullptr || g_state->releasePending || g_state->clearUntil != 0)
            return kPollMs;
        if (g_state->cursorHideAt == 0)
            return -1;
        int64_t left = g_state->cursorHideAt - host::Counter();
        return left <= 0 ? 0 : int(left * 1000 / host::CounterFrequency()) + 1;
    }

    bool IsWakeEvent(const SDL_Event& event)
    {
        return g_wakeEvent != 0 && event.type == g_wakeEvent;
    }

    bool Open(const char* name, Entry entry)
    {
        if (!g_state)
            return false;
        State& state = *g_state;
        if (!state.stack.empty())
            Close();
        state.entry = entry;
        Rml::ElementDocument* document = Load(state, name);
        if (document == nullptr)
        {
            if (state.handlers.closed != nullptr)
                state.handlers.closed(entry);
            return false;
        }
        state.lastFocus.reset();
        state.capture = nullptr;
        pages::Opened();
        ShowOnTop(state, document, false);
        state.openedAt = host::Counter();
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
        Log("ui: %s open (%s)", name, EntryName(entry));
        // The first frame at the next turn of the loop (its presents then start the wakes), not
        // here: Open may be called from inside one of RmlUi's event handlers, where updating the
        // context is not allowed.
        RequestWake();
        return true;
    }

    bool Push(const char* name)
    {
        if (!g_state || g_state->stack.empty())
            return false;
        State& state = *g_state;
        Rml::ElementDocument* document = Load(state, name);
        if (document == nullptr)
            return false;
        state.capture = nullptr;
        ShowOnTop(state, document, true);
        Log("ui: %s over the menu", name);
        RequestWake();
        return true;
    }

    void Close()
    {
        if (!g_state || g_state->stack.empty())
            return;
        State& state = *g_state;
        g_open.store(false, std::memory_order_release);
        // RmlUi unloads the documents at the next context update (the next Open's): closing may come
        // from inside one of its event handlers, where updating the context is not allowed.
        for (auto it = state.stack.rbegin(); it != state.stack.rend(); ++it)
            it->document->Close();
        state.stack.clear();
        state.lastFocus.reset();
        state.pad.Reset();
        state.capture = nullptr;
        pages::Closed();
        Log("ui: closed (%s)", EntryName(state.entry));
        if (state.handlers.closed != nullptr)
            state.handlers.closed(state.entry); // may hold the last frame
        if (state.hold.done == nullptr)
        {
            gpu::ui::Submit({});
            StartRelease(state, host::Counter());
        }
    }

    void HoldLastFrame(bool (*done)(), double timeoutSeconds)
    {
        if (!g_state || !g_state->stack.empty())
            return;
        g_state->hold.done = done;
        g_state->hold.deadline = host::Counter() + host::CounterTicks(timeoutSeconds);
    }

    void Back()
    {
        if (g_state && !g_state->stack.empty())
            Pop(*g_state);
    }

    void CaptureKey(void (*handler)(int scancode))
    {
        if (g_state && !g_state->stack.empty())
            g_state->capture = handler;
    }

    Entry CurrentEntry()
    {
        return g_state ? g_state->entry : Entry::Options;
    }

    bool IsOpen()
    {
        return g_open.load(std::memory_order_acquire);
    }

    void RequestOpen(const char* document, Entry entry)
    {
        if (!IsAvailable())
            return;
        {
            std::lock_guard lock(g_requestMutex);
            g_requests.emplace_back(document, entry);
        }
        RequestWake();
    }

    bool OpenWhen(const char* document, Entry entry, bool (*ready)(), double timeoutSeconds, void (*expired)())
    {
        if (!g_state || !g_state->stack.empty() || g_state->pending || g_state->hold.done != nullptr)
            return false;
        auto pending = std::make_unique<PendingOpen>();
        pending->document = document;
        pending->entry = entry;
        pending->ready = ready;
        pending->expired = expired;
        pending->deadline = host::Counter() + host::CounterTicks(timeoutSeconds);
        g_state->pending = std::move(pending);
        return true;
    }
}
