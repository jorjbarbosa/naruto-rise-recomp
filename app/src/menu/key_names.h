#pragma once

#include <SDL3/SDL_scancode.h>

#include <string_view>

// The SDK's key names for the keys the launcher reads. The game reads the
// keyboard by scancode (the physical key), so the launcher records scancodes
// too: on an ABNT2 keyboard, Ç is "Semicolon", as in the game.
// Mirrors TranslateSDLScancode (sdk/src/ui/sdl_virtual_key.cpp) and the names
// of sdk/src/ui/keybinds.cpp.

// The name of the key at a scancode, or nullptr for a key the game cannot bind
// (the Windows keys, the numpad's decimal point, keys outside a US layout).
const char* KeyNameForScancode(SDL_Scancode scancode);
// A scancode of the key with that name; SDL_SCANCODE_UNKNOWN for none and for
// the mouse buttons.
SDL_Scancode ScancodeForKeyName(std::string_view name);
// "LMB", "RMB" or "MMB" for SDL's left, right and middle buttons; nullptr for
// the others.
const char* KeyNameForMouseButton(int sdl_button);
