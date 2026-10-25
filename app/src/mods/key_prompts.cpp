#include "mods/key_prompts.h"

#include <array>
#include <utility>

namespace narutorise::key_prompts {
namespace {

struct TagBind {
  std::string_view tag;
  std::string_view option;
};

// Button tags found in the English text tables and the input each stands for.
// The stick-direction tags name two sticks: in jutsu texts, which also hold
// <lt> or <lb> ("<lt> <analogright><analogleft> <analogup><analogup>"), the
// right stick; in combat combos ("Press <analogleft> <x> <x> <x>") and the
// combo tags ("<x-left>"), the left one, kept under the "move:" keys.
constexpr std::string_view kMovePrefix = "move:";
constexpr std::array<TagBind, 18> kSingleTags = {{
    {"a", "keybind_a"},
    {"b", "keybind_b"},
    {"x", "keybind_x"},
    {"y", "keybind_y"},
    {"lb", "keybind_left_shoulder"},
    {"rb", "keybind_right_shoulder"},
    {"lt", "keybind_left_trigger"},
    {"rt", "keybind_right_trigger"},
    {"start", "keybind_start"},
    {"back", "keybind_back"},
    {"analogup", "keybind_rstick_up"},
    {"analogdown", "keybind_rstick_down"},
    {"analogleft", "keybind_rstick_left"},
    {"analogright", "keybind_rstick_right"},
    {"move:up", "keybind_lstick_up"},
    {"move:down", "keybind_lstick_down"},
    {"move:left", "keybind_lstick_left"},
    {"move:right", "keybind_lstick_right"},
}};

// Tags naming a whole stick or pad: shown as its four keys, the stick as
// up/left/down/right ("W/A/S/D"), the pad as up/down/left/right ("1/2/3/4"),
// the order docs/configuration.md lists them in.
struct TagGroup {
  std::string_view tag;
  std::array<std::string_view, 4> options;
};
constexpr std::array<TagGroup, 2> kGroupTags = {{
    {"leftanalog",
     {"keybind_lstick_up", "keybind_lstick_left", "keybind_lstick_down", "keybind_lstick_right"}},
    {"dpad", {"keybind_dpad_up", "keybind_dpad_down", "keybind_dpad_left", "keybind_dpad_right"}},
}};

constexpr std::array<std::string_view, 22> kOptions = {
    "keybind_a",           "keybind_b",             "keybind_x",
    "keybind_y",           "keybind_left_shoulder", "keybind_right_shoulder",
    "keybind_left_trigger", "keybind_right_trigger", "keybind_start",
    "keybind_back",        "keybind_lstick_up",     "keybind_lstick_down",
    "keybind_lstick_left", "keybind_lstick_right",  "keybind_rstick_up",
    "keybind_rstick_down", "keybind_rstick_left",   "keybind_rstick_right",
    "keybind_dpad_up",     "keybind_dpad_down",     "keybind_dpad_left",
    "keybind_dpad_right",
};

// Display text for the SDK key names (sdk/src/ui/keybinds.cpp) that are not
// readable as they are. Kept to ASCII: the game's fonts lack most symbols.
constexpr std::pair<std::string_view, std::string_view> kLabels[] = {
    {"LMB", "Left Click"},
    {"RMB", "Right Click"},
    {"MMB", "Middle Click"},
    {"Return", "Enter"},
    {"Escape", "Esc"},
    {"Control", "Ctrl"},
    {"Backtick", "`"},
    {"Minus", "-"},
    {"Plus", "="},
    {"Comma", ","},
    {"Period", "."},
    {"Semicolon", ";"},
    {"Slash", "/"},
    {"Backslash", "\\"},
    {"LBracket", "Left Bracket"},
    {"RBracket", "Right Bracket"},
    {"Quote", "'"},
    {"PageUp", "Page Up"},
    {"PageDown", "Page Down"},
    {"CapsLock", "Caps Lock"},
    {"NumLock", "Num Lock"},
    {"ScrollLock", "Scroll Lock"},
    {"PrintScreen", "Print Screen"},
    {"NumpadPlus", "Num +"},
    {"NumpadMinus", "Num -"},
    {"NumpadStar", "Num *"},
    {"NumpadSlash", "Num /"},
};

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
    text.remove_suffix(1);
  return text;
}

bool EqualsNoCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z')
      x = char(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z')
      y = char(y - 'A' + 'a');
    if (x != y)
      return false;
  }
  return true;
}

std::string SingleKeyLabel(std::string_view name) {
  for (const auto& [key, label] : kLabels) {
    if (name == key)
      return std::string(label);
  }
  // "Numpad7" -> "Num 7"
  constexpr std::string_view kNumpad = "Numpad";
  if (name.size() == kNumpad.size() + 1 && name.starts_with(kNumpad) && name.back() >= '0' &&
      name.back() <= '9')
    return "Num " + std::string(1, name.back());
  return std::string(name);
}

std::string LabelOf(const BindValues& binds, std::string_view option) {
  const auto found = binds.find(option);
  if (found == binds.end())
    return {};
  const auto key = FirstKey(found->second);
  return key.empty() ? std::string{} : KeyLabel(key);
}

}  // namespace

std::span<const std::string_view> BindOptions() {
  return kOptions;
}

std::string KeyLabel(std::string_view key_name) {
  std::string out;
  for (;;) {
    const size_t plus = key_name.find('+');
    // A trailing or lone '+' is not a combination separator.
    if (plus == std::string_view::npos || plus == 0 || plus + 1 == key_name.size()) {
      out += SingleKeyLabel(Trim(key_name));
      return out;
    }
    out += SingleKeyLabel(Trim(key_name.substr(0, plus)));
    out += '+';
    key_name.remove_prefix(plus + 1);
  }
}

std::string FirstKey(std::string_view value) {
  while (!value.empty()) {
    const size_t comma = value.find(',');
    const auto part = Trim(value.substr(0, comma));
    if (!part.empty() && !EqualsNoCase(part, "None"))
      return std::string(part);
    if (comma == std::string_view::npos)
      break;
    value.remove_prefix(comma + 1);
  }
  return {};
}

Labels BuildLabels(const BindValues& binds) {
  Labels labels;
  for (const auto& entry : kSingleTags) {
    auto label = LabelOf(binds, entry.option);
    if (!label.empty())
      labels.emplace(std::string(entry.tag), std::move(label));
  }
  for (const auto& group : kGroupTags) {
    std::string joined;
    bool complete = true;
    for (const auto option : group.options) {
      const auto label = LabelOf(binds, option);
      if (label.empty()) {
        complete = false;
        break;
      }
      if (!joined.empty())
        joined += '/';
      joined += label;
    }
    if (complete)
      labels.emplace(std::string(group.tag), std::move(joined));
  }
  return labels;
}

std::string ReplaceButtonTags(std::string_view text, const Labels& labels) {
  // A jutsu text holds its trigger; its stick motions are the right stick's.
  const bool jutsu = text.find("<lt>") != std::string_view::npos ||
                     text.find("<lb>") != std::string_view::npos;
  const auto find = [&](std::string_view key) -> const std::string* {
    const auto found = labels.find(key);
    return found == labels.end() ? nullptr : &found->second;
  };
  // The label of one tag; empty when it keeps its icon.
  const auto label_of = [&](std::string_view name) -> std::string {
    constexpr std::string_view kAnalog = "analog";
    if (name.starts_with(kAnalog) && !jutsu && name != "analog") {
      const auto* move = find(std::string(kMovePrefix) + std::string(name.substr(kAnalog.size())));
      return move ? *move : std::string{};
    }
    // Combo input: a direction held with a button ("<x-left>").
    if (const size_t dash = name.find('-'); dash != std::string_view::npos) {
      const auto* button = find(name.substr(0, dash));
      const auto* move = find(std::string(kMovePrefix) + std::string(name.substr(dash + 1)));
      return button && move ? *move + "+" + *button : std::string{};
    }
    if (name.starts_with(kMovePrefix))
      return {};  // Internal keys are not tags.
    const auto* label = find(name);
    return label ? *label : std::string{};
  };

  std::string out;
  out.reserve(text.size());
  size_t at = 0;
  while (at < text.size()) {
    const size_t open = text.find('<', at);
    if (open == std::string_view::npos)
      break;
    const size_t close = text.find('>', open + 1);
    if (close == std::string_view::npos)
      break;
    out.append(text.substr(at, open - at));
    const auto label = label_of(text.substr(open + 1, close - open - 1));
    if (!label.empty()) {
      out += '[';
      out += label;
      out += ']';
    } else {
      out.append(text.substr(open, close - open + 1));
    }
    at = close + 1;
  }
  out.append(text.substr(at));
  return out;
}

}  // namespace narutorise::key_prompts
