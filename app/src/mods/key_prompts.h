#pragma once

// Keyboard prompts: with keyboard and mouse on (mnk_mode), the game's button
// icons in text become the keys bound to them. The game writes a controller
// button in its text as an engine tag ("Press <a> to jump"); these functions
// replace each such tag with the first key of its keybind_* option
// ("Press [Space] to jump"). Colour, line break and currency tags stay.
//
// Pure functions without SDK dependencies, so the host test can check them.

#include <map>
#include <span>
#include <string>
#include <string_view>

namespace narutorise::key_prompts {

// keybind_* option name -> value ("keybind_a" -> "Space,E").
using BindValues = std::map<std::string, std::string, std::less<>>;
// Tag name without brackets ("a", "lt", "analogup") -> text shown instead.
using Labels = std::map<std::string, std::string, std::less<>>;

// The keybind_* options the labels read.
std::span<const std::string_view> BindOptions();

// The text a key name is shown as: "LMB" -> "Left Click", "Return" -> "Enter",
// "Shift+Up" -> "Shift+Up". Unknown names are shown unchanged.
std::string KeyLabel(std::string_view key_name);

// The first non-empty key of a comma-separated keybind value; empty for an
// unbound input ("", "None").
std::string FirstKey(std::string_view value);

// The label of every button tag whose input has a key. A tag whose input is
// unbound gets no label and keeps its icon.
Labels BuildLabels(const BindValues& binds);

// The text with each labelled button tag replaced by "[label]". Stick
// directions ("<analogleft>") are the right stick's in jutsu texts (those with
// <lt> or <lb>) and the left stick's elsewhere; a combo tag ("<x-left>") is a
// left-stick direction held with a button ("[A+Left Click]"). Other tags,
// unknown tags and unterminated '<' are kept byte for byte.
std::string ReplaceButtonTags(std::string_view text, const Labels& labels);

}  // namespace narutorise::key_prompts
