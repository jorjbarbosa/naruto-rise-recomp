#include "key_bindings.h"

namespace {

struct KeyAction {
  const char* option;
  const char* default_value;
};

// The default layout; the SDK's key names (sdk/src/ui/keybinds.cpp).
constexpr KeyAction kActions[kKeyActionCount] = {
    {"keybind_a", "Space"},
    {"keybind_b", "E,Backspace"},
    {"keybind_x", "LMB,J"},
    {"keybind_y", "RMB,K"},
    {"keybind_left_shoulder", "Q"},
    {"keybind_right_shoulder", "R"},
    {"keybind_left_trigger", "Control"},
    {"keybind_right_trigger", "Shift"},
    {"keybind_lstick_press", "C"},
    {"keybind_rstick_press", "MMB,V"},
    {"keybind_start", "Return,Escape"},
    {"keybind_back", "Tab"},
    {"keybind_lstick_up", "W"},
    {"keybind_lstick_down", "S"},
    {"keybind_lstick_left", "A"},
    {"keybind_lstick_right", "D"},
    {"keybind_rstick_up", "Up"},
    {"keybind_rstick_down", "Down"},
    {"keybind_rstick_left", "Left"},
    {"keybind_rstick_right", "Right"},
    {"keybind_dpad_up", "1,Numpad8"},
    {"keybind_dpad_down", "2,Numpad2"},
    {"keybind_dpad_left", "3,Numpad4"},
    {"keybind_dpad_right", "4,Numpad6"},
};

constexpr const char* kButtonNames[kKeyButtonCount] = {
    "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "L3", "R3", "Start", "Back",
};

std::string_view Trim(std::string_view text) {
  while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
  while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
  return text;
}

}  // namespace

const char* KeyActionOption(std::size_t action) {
  return action < kKeyActionCount ? kActions[action].option : "";
}

const char* KeyButtonName(std::size_t action) {
  return action < kKeyButtonCount ? kButtonNames[action] : "";
}

KeyBinds DefaultKeyBinds() {
  KeyBinds binds;
  for (std::size_t i = 0; i < kKeyActionCount; ++i) binds[i] = kActions[i].default_value;
  return binds;
}

std::array<std::string, 2> SplitKeyBind(std::string_view value) {
  std::array<std::string, 2> keys;
  for (std::size_t index = 0; index < keys.size(); ++index) {
    const std::size_t comma = value.find(',');
    keys[index] = Trim(value.substr(0, comma));
    if (comma == std::string_view::npos) break;
    value.remove_prefix(comma + 1);
  }
  return keys;
}

std::string JoinKeyBind(std::string_view first, std::string_view second) {
  std::string value(first);
  if (!second.empty()) {
    value += ',';
    value += second;
  }
  return value;
}

bool IsReservedKey(std::string_view name) {
  for (const char* reserved : {"F1", "F3", "F4", "F7", "F11", "Backtick"}) {
    if (name == reserved) return true;
  }
  return false;
}
