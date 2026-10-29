#include "key_names.h"

#include <SDL3/SDL_mouse.h>

#include <iterator>

namespace {

struct KeyName {
  SDL_Scancode scancode;
  const char* name;
};

// Several scancodes can share a name (both Shift keys, both Enter keys); the
// first one stands for the name.
constexpr KeyName kKeys[] = {
    {SDL_SCANCODE_A, "A"}, {SDL_SCANCODE_B, "B"}, {SDL_SCANCODE_C, "C"}, {SDL_SCANCODE_D, "D"},
    {SDL_SCANCODE_E, "E"}, {SDL_SCANCODE_F, "F"}, {SDL_SCANCODE_G, "G"}, {SDL_SCANCODE_H, "H"},
    {SDL_SCANCODE_I, "I"}, {SDL_SCANCODE_J, "J"}, {SDL_SCANCODE_K, "K"}, {SDL_SCANCODE_L, "L"},
    {SDL_SCANCODE_M, "M"}, {SDL_SCANCODE_N, "N"}, {SDL_SCANCODE_O, "O"}, {SDL_SCANCODE_P, "P"},
    {SDL_SCANCODE_Q, "Q"}, {SDL_SCANCODE_R, "R"}, {SDL_SCANCODE_S, "S"}, {SDL_SCANCODE_T, "T"},
    {SDL_SCANCODE_U, "U"}, {SDL_SCANCODE_V, "V"}, {SDL_SCANCODE_W, "W"}, {SDL_SCANCODE_X, "X"},
    {SDL_SCANCODE_Y, "Y"}, {SDL_SCANCODE_Z, "Z"},
    {SDL_SCANCODE_1, "1"}, {SDL_SCANCODE_2, "2"}, {SDL_SCANCODE_3, "3"}, {SDL_SCANCODE_4, "4"},
    {SDL_SCANCODE_5, "5"}, {SDL_SCANCODE_6, "6"}, {SDL_SCANCODE_7, "7"}, {SDL_SCANCODE_8, "8"},
    {SDL_SCANCODE_9, "9"}, {SDL_SCANCODE_0, "0"},
    {SDL_SCANCODE_F1, "F1"}, {SDL_SCANCODE_F2, "F2"}, {SDL_SCANCODE_F3, "F3"}, {SDL_SCANCODE_F4, "F4"},
    {SDL_SCANCODE_F5, "F5"}, {SDL_SCANCODE_F6, "F6"}, {SDL_SCANCODE_F7, "F7"}, {SDL_SCANCODE_F8, "F8"},
    {SDL_SCANCODE_F9, "F9"}, {SDL_SCANCODE_F10, "F10"}, {SDL_SCANCODE_F11, "F11"},
    {SDL_SCANCODE_F12, "F12"}, {SDL_SCANCODE_F13, "F13"}, {SDL_SCANCODE_F14, "F14"},
    {SDL_SCANCODE_F15, "F15"}, {SDL_SCANCODE_F16, "F16"}, {SDL_SCANCODE_F17, "F17"},
    {SDL_SCANCODE_F18, "F18"}, {SDL_SCANCODE_F19, "F19"}, {SDL_SCANCODE_F20, "F20"},
    {SDL_SCANCODE_F21, "F21"}, {SDL_SCANCODE_F22, "F22"}, {SDL_SCANCODE_F23, "F23"},
    {SDL_SCANCODE_F24, "F24"},
    {SDL_SCANCODE_KP_0, "Numpad0"}, {SDL_SCANCODE_KP_1, "Numpad1"}, {SDL_SCANCODE_KP_2, "Numpad2"},
    {SDL_SCANCODE_KP_3, "Numpad3"}, {SDL_SCANCODE_KP_4, "Numpad4"}, {SDL_SCANCODE_KP_5, "Numpad5"},
    {SDL_SCANCODE_KP_6, "Numpad6"}, {SDL_SCANCODE_KP_7, "Numpad7"}, {SDL_SCANCODE_KP_8, "Numpad8"},
    {SDL_SCANCODE_KP_9, "Numpad9"},
    {SDL_SCANCODE_KP_MULTIPLY, "NumpadStar"}, {SDL_SCANCODE_KP_PLUS, "NumpadPlus"},
    {SDL_SCANCODE_KP_MINUS, "NumpadMinus"}, {SDL_SCANCODE_KP_DIVIDE, "NumpadSlash"},
    // The numpad's Enter is the same key as Enter for the game.
    {SDL_SCANCODE_RETURN, "Return"}, {SDL_SCANCODE_KP_ENTER, "Return"},
    {SDL_SCANCODE_ESCAPE, "Escape"}, {SDL_SCANCODE_BACKSPACE, "Backspace"}, {SDL_SCANCODE_TAB, "Tab"},
    {SDL_SCANCODE_SPACE, "Space"},
    {SDL_SCANCODE_LEFT, "Left"}, {SDL_SCANCODE_UP, "Up"}, {SDL_SCANCODE_RIGHT, "Right"},
    {SDL_SCANCODE_DOWN, "Down"},
    {SDL_SCANCODE_PAGEUP, "PageUp"}, {SDL_SCANCODE_PAGEDOWN, "PageDown"}, {SDL_SCANCODE_HOME, "Home"},
    {SDL_SCANCODE_END, "End"}, {SDL_SCANCODE_INSERT, "Insert"}, {SDL_SCANCODE_DELETE, "Delete"},
    // Left and right are one key each for the game.
    {SDL_SCANCODE_LSHIFT, "Shift"}, {SDL_SCANCODE_RSHIFT, "Shift"},
    {SDL_SCANCODE_LCTRL, "Control"}, {SDL_SCANCODE_RCTRL, "Control"},
    {SDL_SCANCODE_LALT, "Alt"}, {SDL_SCANCODE_RALT, "Alt"},
    {SDL_SCANCODE_CAPSLOCK, "CapsLock"}, {SDL_SCANCODE_PAUSE, "Pause"},
    {SDL_SCANCODE_PRINTSCREEN, "PrintScreen"}, {SDL_SCANCODE_SCROLLLOCK, "ScrollLock"},
    {SDL_SCANCODE_NUMLOCKCLEAR, "NumLock"},
    {SDL_SCANCODE_SEMICOLON, "Semicolon"}, {SDL_SCANCODE_EQUALS, "Plus"}, {SDL_SCANCODE_COMMA, "Comma"},
    {SDL_SCANCODE_MINUS, "Minus"}, {SDL_SCANCODE_PERIOD, "Period"}, {SDL_SCANCODE_SLASH, "Slash"},
    {SDL_SCANCODE_GRAVE, "Backtick"}, {SDL_SCANCODE_LEFTBRACKET, "LBracket"},
    {SDL_SCANCODE_BACKSLASH, "Backslash"}, {SDL_SCANCODE_RIGHTBRACKET, "RBracket"},
    {SDL_SCANCODE_APOSTROPHE, "Quote"},
};

}  // namespace

const char* KeyNameForScancode(SDL_Scancode scancode) {
  for (const auto& key : kKeys) {
    if (key.scancode == scancode) return key.name;
  }
  return nullptr;
}

SDL_Scancode ScancodeForKeyName(std::string_view name) {
  for (const auto& key : kKeys) {
    if (name == key.name) return key.scancode;
  }
  return SDL_SCANCODE_UNKNOWN;
}

const char* KeyNameForMouseButton(int sdl_button) {
  switch (sdl_button) {
    case SDL_BUTTON_LEFT: return "LMB";
    case SDL_BUTTON_RIGHT: return "RMB";
    case SDL_BUTTON_MIDDLE: return "MMB";
    default: return nullptr;
  }
}
