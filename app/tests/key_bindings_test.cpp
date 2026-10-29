#include "menu/key_bindings.h"
#include "menu/key_names.h"

#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

// The SDK's key names (kKeyNames in sdk/src/ui/keybinds.cpp). The game parses
// only these; a name outside them would bind nothing.
const std::set<std::string_view> kSdkNames = {
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
    "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "Backtick", "Minus", "Plus", "Comma", "Period", "Semicolon", "Slash", "Backslash",
    "LBracket", "RBracket", "Quote",
    "Escape", "Return", "Space", "Tab", "Backspace", "Delete", "Insert", "Home", "End",
    "PageUp", "PageDown", "Left", "Right", "Up", "Down", "Shift", "Control", "Alt",
    "Numpad0", "Numpad1", "Numpad2", "Numpad3", "Numpad4", "Numpad5", "Numpad6", "Numpad7",
    "Numpad8", "Numpad9", "NumpadEnter", "NumpadPlus", "NumpadMinus", "NumpadStar", "NumpadSlash",
    "PrintScreen", "Pause", "CapsLock", "NumLock", "ScrollLock", "LMB", "RMB", "MMB",
};

}  // namespace

int main() {
  try {
    // Every key the launcher can record is a name the game parses, and each
    // name leads back to a key with that name.
    int named = 0;
    for (int code = 0; code < SDL_SCANCODE_COUNT; ++code) {
      const char* name = KeyNameForScancode(static_cast<SDL_Scancode>(code));
      if (!name) continue;
      ++named;
      Require(kSdkNames.contains(name), std::string("Not an SDK key name: ") + name);
      Require(std::string_view(KeyNameForScancode(ScancodeForKeyName(name))) == name,
              std::string("Name does not round-trip: ") + name);
    }
    Require(named >= 100, "Too few keys can be recorded");
    // The physical key, as the game reads it: Ç on ABNT2 sits where ; is.
    Require(std::string_view(KeyNameForScancode(SDL_SCANCODE_SEMICOLON)) == "Semicolon", "Semicolon");
    Require(std::string_view(KeyNameForScancode(SDL_SCANCODE_KP_ENTER)) == "Return", "Numpad Enter is Enter");
    Require(std::string_view(KeyNameForScancode(SDL_SCANCODE_RSHIFT)) == "Shift", "Right Shift is Shift");
    Require(std::string_view(KeyNameForScancode(SDL_SCANCODE_RALT)) == "Alt", "Right Alt is Alt");
    Require(KeyNameForScancode(SDL_SCANCODE_LGUI) == nullptr, "The Windows key cannot be bound");
    Require(KeyNameForScancode(SDL_SCANCODE_KP_PERIOD) == nullptr, "The numpad point has no SDK name");
    Require(ScancodeForKeyName("LMB") == SDL_SCANCODE_UNKNOWN, "Mouse buttons have no scancode");
    Require(std::string_view(KeyNameForMouseButton(1)) == "LMB" &&
                std::string_view(KeyNameForMouseButton(2)) == "MMB" &&
                std::string_view(KeyNameForMouseButton(3)) == "RMB" && KeyNameForMouseButton(4) == nullptr,
            "Mouse buttons");

    // The default layout: SDK names only, no key twice, no reserved key.
    const KeyBinds defaults = DefaultKeyBinds();
    std::set<std::string> used;
    std::set<std::string> options;
    for (std::size_t i = 0; i < kKeyActionCount; ++i) {
      const std::string option = KeyActionOption(i);
      Require(option.starts_with("keybind_") && options.insert(option).second, "Option name: " + option);
      for (const auto& key : SplitKeyBind(defaults[i])) {
        if (key.empty()) continue;
        Require(kSdkNames.contains(key), "Default is not an SDK name: " + key);
        Require(!IsReservedKey(key), "Default is reserved: " + key);
        Require(used.insert(key).second, "Default key used twice: " + key);
      }
    }
    // The automated boot routes press Enter (Start), Space (A), W/S and the arrows.
    Require(defaults[10] == "Return,Escape" && defaults[0] == "Space" && defaults[12] == "W" &&
                defaults[13] == "S" && defaults[18] == "Left" && defaults[19] == "Right",
            "The keys of the boot routes changed");

    // Two keys by position.
    Require((SplitKeyBind("E,Backspace") == std::array<std::string, 2>{"E", "Backspace"}), "Split two");
    Require((SplitKeyBind(",Q") == std::array<std::string, 2>{"", "Q"}), "Split empty first");
    Require((SplitKeyBind(" W ") == std::array<std::string, 2>{"W", ""}), "Split one, trimmed");
    Require((SplitKeyBind("A,B,C") == std::array<std::string, 2>{"A", "B"}), "Split keeps two");
    Require((SplitKeyBind("") == std::array<std::string, 2>{"", ""}), "Split empty");
    Require((SplitKeyBind("Shift+Up") == std::array<std::string, 2>{"Shift+Up", ""}), "Split combination");
    Require(JoinKeyBind("", "Q") == ",Q" && JoinKeyBind("Q", "") == "Q" && JoinKeyBind("", "") == "" &&
                JoinKeyBind("E", "Backspace") == "E,Backspace",
            "Join");
    for (const auto& value : defaults) {
      const auto keys = SplitKeyBind(value);
      Require(JoinKeyBind(keys[0], keys[1]) == value, "Default does not round-trip: " + value);
    }

    for (const char* key : {"F1", "F3", "F4", "F7", "F11", "Backtick"}) Require(IsReservedKey(key), key);
    for (const char* key : {"F2", "F9", "Escape", "Alt"}) Require(!IsReservedKey(key), key);

    std::cout << named << " recordable keys, all SDK names; default layout valid; values split by position\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
