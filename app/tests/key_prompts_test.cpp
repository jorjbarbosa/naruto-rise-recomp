#include <iostream>
#include <stdexcept>
#include <string>

#include "mods/key_prompts.h"

using namespace narutorise::key_prompts;
namespace {
unsigned checks = 0;
void Check(bool value, const char* label) {
  ++checks;
  if (!value)
    throw std::runtime_error(label);
}
void Equal(const std::string& actual, const std::string& expected, const char* label) {
  ++checks;
  if (actual != expected)
    throw std::runtime_error(std::string(label) + ": got \"" + actual + "\", expected \"" +
                             expected + "\"");
}

// The launcher's default layout (launcher/src/key_bindings.cpp).
BindValues LauncherDefaults() {
  return {
      {"keybind_a", "Space"},
      {"keybind_b", "E,Backspace"},
      {"keybind_x", "LMB,J"},
      {"keybind_y", "RMB,K"},
      {"keybind_left_shoulder", "Q"},
      {"keybind_right_shoulder", "R"},
      {"keybind_left_trigger", "Control"},
      {"keybind_right_trigger", "Shift"},
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
}

void KeyLabelTests() {
  Equal(KeyLabel("Space"), "Space", "plain name");
  Equal(KeyLabel("LMB"), "Left Click", "mouse button");
  Equal(KeyLabel("Return"), "Enter", "return");
  Equal(KeyLabel("Control"), "Ctrl", "control");
  Equal(KeyLabel("Semicolon"), ";", "punctuation");
  Equal(KeyLabel("Numpad7"), "Num 7", "numpad digit");
  Equal(KeyLabel("NumpadPlus"), "Num +", "numpad operator");
  Equal(KeyLabel("Shift+Up"), "Shift+Up", "combination");
  Equal(KeyLabel("Control+Quote"), "Ctrl+'", "combination labels each key");
  Equal(KeyLabel("Mystery"), "Mystery", "unknown name kept");
}

void FirstKeyTests() {
  Equal(FirstKey("E,Backspace"), "E", "first of two");
  Equal(FirstKey(" ,Q"), "Q", "empty first alternative skipped");
  Equal(FirstKey("None"), "", "None is unbound");
  Equal(FirstKey("none, W"), "W", "None skipped case-insensitively");
  Equal(FirstKey(""), "", "empty value");
  Equal(FirstKey(" Shift+Up "), "Shift+Up", "trimmed");
}

void LabelTests() {
  const auto labels = BuildLabels(LauncherDefaults());
  Equal(labels.at("a"), "Space", "A");
  Equal(labels.at("x"), "Left Click", "X");
  Equal(labels.at("lt"), "Ctrl", "LT");
  Equal(labels.at("start"), "Enter", "Start");
  Equal(labels.at("analogup"), "Up", "jutsu motions use the right stick");
  Equal(labels.at("move:left"), "A", "combat directions use the left stick");
  Equal(labels.at("leftanalog"), "W/A/S/D", "left stick as one label");
  Equal(labels.at("dpad"), "1/2/3/4", "d-pad as one label, up/down/left/right");

  auto partial = LauncherDefaults();
  partial["keybind_y"] = "None";
  partial["keybind_lstick_left"] = "";
  partial.erase("keybind_back");
  const auto some = BuildLabels(partial);
  Check(!some.contains("y"), "unbound button must keep its icon");
  Check(!some.contains("back"), "missing option must keep its icon");
  Check(!some.contains("leftanalog"), "a stick missing a direction must keep its icon");
  Check(some.contains("a"), "other buttons still labelled");
  Check(BuildLabels({}).empty(), "no binds, no labels");
}

void ReplaceTests() {
  const auto labels = BuildLabels(LauncherDefaults());
  Equal(ReplaceButtonTags("Press <a> to jump.", labels), "Press [Space] to jump.", "single tag");
  Equal(ReplaceButtonTags("<yellow>Press <a> twice<white>", labels),
        "<yellow>Press [Space] twice<white>", "colour tags kept");
  Equal(ReplaceButtonTags("<lt> <analogright><analogleft> <analogup><analogup>.", labels),
        "[Ctrl] [Right][Left] [Up][Up].", "jutsu motion uses the right stick");
  Equal(ReplaceButtonTags("<lb><analogright><analogleft>", labels), "[Q][Right][Left]",
        "chakra motion after LB uses the right stick");
  Equal(ReplaceButtonTags("Press <analogleft> <x> <x> <x> for a combo.", labels),
        "Press [A] [Left Click] [Left Click] [Left Click] for a combo.",
        "combat direction uses the left stick");
  Equal(ReplaceButtonTags("<x-left> <x> <y>", labels), "[A+Left Click] [Left Click] [Right Click]",
        "combo tag is a direction held with a button");
  Equal(ReplaceButtonTags("<y-up> <y> <x>", labels), "[W+Right Click] [Right Click] [Left Click]",
        "combo tag up");
  Equal(ReplaceButtonTags("<z-left> <x-sideways> <move:up>", labels),
        "<z-left> <x-sideways> <move:up>", "unknown combo parts and internal keys kept");
  Equal(ReplaceButtonTags("Coins <goldcoin> and <br> lines", labels), "Coins <goldcoin> and <br> lines",
        "non-button tags kept");
  Equal(ReplaceButtonTags("Steer with <leftanalog>, pick with <dpad>.", labels),
        "Steer with [W/A/S/D], pick with [1/2/3/4].", "group tags");
  Equal(ReplaceButtonTags("<A> <x", labels), "<A> <x", "case-sensitive, unterminated kept");
  Equal(ReplaceButtonTags("a > b < c", labels), "a > b < c", "stray brackets kept");
  Equal(ReplaceButtonTags("", labels), "", "empty text");
  Equal(ReplaceButtonTags("Press <a>.", {}), "Press <a>.", "no labels, no change");
}
}  // namespace

int main() {
  try {
    KeyLabelTests();
    FirstKeyTests();
    LabelTests();
    ReplaceTests();
    std::cout << "key prompts PASS checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "key prompts FAIL after " << checks << " checks: " << error.what() << '\n';
    return 1;
  }
}
