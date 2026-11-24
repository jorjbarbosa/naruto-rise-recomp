#pragma once

#include <cstdint>

union SDL_Event;

// Gamepads (any SDL 3 gamepad) and the keyboard, merged into the four XInput player slots.
//
// Gamepads take slots 0-3 in connection order and keep them until they are removed; the keyboard
// merges into slot 0, which is therefore always connected. Events are handled on the host main
// thread (HandleInputEvent, from RunEventLoop); the queries copy the slot state under a mutex and
// may run on any thread.
namespace host
{
    constexpr int kInputSlots = 4;

    // One slot in the X_INPUT_GAMEPAD layout, host byte order: XDK button bits, triggers 0-255,
    // sticks with up positive, after the host dead zone of gamepad.dead_zone (0: none; the game
    // applies its own after it).
    struct PadState
    {
        // Changes whenever any other field changes: the game reads the rest only on a new packet.
        uint32_t packet = 0;
        uint16_t buttons = 0;
        uint8_t leftTrigger = 0;
        uint8_t rightTrigger = 0;
        int16_t thumbLX = 0;
        int16_t thumbLY = 0;
        int16_t thumbRX = 0;
        int16_t thumbRY = 0;
    };

    // Takes the keyboard bindings (keyboard.*) and the dead zone from the store (settings.h). Host
    // main thread, after SDL_Init with SDL_INIT_GAMEPAD; again after a change.
    void ApplyInputSettings();
    // Feeds one SDL event to the input state. Host main thread only, after SDL_Init with
    // SDL_INIT_GAMEPAD; events that are not about input are ignored.
    void HandleInputEvent(const SDL_Event& event);
    // Copies the state of a slot. False when nothing is connected to it. Any thread.
    bool GetPadState(int slot, PadState& out);
    // Holds or lets go of a keyboard key, named as SDL names it ("Return", "Space", "S"), as if typed:
    // for scripted runs (gpu/guest/compare.h). False for a name SDL does not know. Any thread.
    bool SetScriptedKey(const char* name, bool down);
    // Sets the rumble motors of a slot (0-65535 each); they keep running until changed. Muted while
    // the window is not focused. False when nothing is connected to the slot. Any thread.
    bool SetPadRumble(int slot, uint16_t left, uint16_t right);
    // Stops every motor for good: some controllers keep the last rumble after the process is gone.
    // Call it before exiting the process. Any thread.
    void StopInput();
}
