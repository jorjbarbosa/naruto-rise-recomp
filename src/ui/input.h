#pragma once

// SDL input for the host UI: mouse and keyboard turned into RmlUi's, and the menu actions, which are
// the same on every device and independent of the game's [keyboard] table, so the menu stays usable
// whatever the bindings. Host main thread only.

#include <RmlUi/Core/Input.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

union SDL_Event;

namespace ui
{
    enum class Action
    {
        Up,
        Down,
        Left,
        Right,
        Accept,
        Back,
        PreviousTab,
        NextTab,
        Reset,
        // The pad's Guide button: the port menu's own key (the game cannot read it).
        Menu,
    };

    // Keyboard: arrows; Enter, keypad Enter, Space; Escape, Backspace; Q, E, Page Up, Page Down;
    // Delete. None for any other key. The directions repeat with the system's key repeat; the others
    // act once per press.
    bool KeyAction(int scancode, Action& action);
    // RmlUi's key for an SDL scancode (KI_UNKNOWN when it has none), and its modifier state.
    Rml::Input::KeyIdentifier KeyIdentifier(int scancode);
    int KeyModifiers(uint16_t sdlModifiers);

    bool IsDirection(Action action);

    // The pads' menu actions: the d-pad and the left stick (past half its travel, released below 35%)
    // as directions that repeat while held, A, B, LB, RB, X (Reset) and Guide. Every pad drives them; a direction is
    // held while any pad holds it.
    class PadActions
    {
    public:
        // Takes a gamepad event; other events are ignored. Appends the actions it starts.
        void HandleEvent(const SDL_Event& event, int64_t now, std::vector<Action>& actions);
        // Appends the repeats of the held direction due by now (host::Counter ticks).
        void Tick(int64_t now, std::vector<Action>& actions);
        // Forgets what is held (focus lost, a document closed).
        void Reset();
        // What is held now does not repeat (a document opening over a direction held in the game);
        // a new press does.
        void SuppressRepeat();

    private:
        struct Pad
        {
            uint32_t dpad = 0; // held directions, a bit per Action::Up..Right
            uint32_t stick = 0;
            float stickX = 0.0f;
            float stickY = 0.0f;
        };

        // Recomputes the directions all pads hold together, starting the newly pressed ones.
        void UpdateHeld(int64_t now, std::vector<Action>& actions);

        std::unordered_map<uint32_t, Pad> pads_; // by SDL joystick id
        uint32_t held_ = 0;
        int repeating_ = -1; // the direction that repeats, the last one pressed
        int64_t nextRepeat_ = 0;
    };
}
