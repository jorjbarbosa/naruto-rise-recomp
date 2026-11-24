#pragma once

// Holds the game's input while a host menu is open, the way the engine's own message box does: a bit
// of its input flag (game::jade::kInputFlag) that no game code touches. The pads and the keyboard
// keep feeding XInput; the game reads nothing through the flag.
namespace game
{
    // Sets or clears the host's bit. Idempotent; any thread. Setting it before the game zeroes the
    // flag at start loses it, so a holder sets it again while it holds.
    void SetInputHeld(bool held);
}
