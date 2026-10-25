#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

// The controller inputs the keyboard and mouse stand for in the game's
// keyboard and mouse mode (mnk_mode), one keybind_* option each. A value is a
// comma-separated list of the SDK's key names ("E,Backspace"); the launcher
// edits the first two of them.
//
// Order: the 12 buttons, then the left stick, the right stick and the D-pad,
// four directions each (up, down, left, right).
inline constexpr std::size_t kKeyActionCount = 24;
inline constexpr std::size_t kKeyButtonCount = 12;
inline constexpr std::size_t kLeftStickFirst = 12;
inline constexpr std::size_t kRightStickFirst = 16;
inline constexpr std::size_t kDpadFirst = 20;

using KeyBinds = std::array<std::string, kKeyActionCount>;

// The game option of an input ("keybind_a").
const char* KeyActionOption(std::size_t action);
// The controller's name for one of the 12 buttons ("A", "LB", "Start").
const char* KeyButtonName(std::size_t action);

// The default layout.
KeyBinds DefaultKeyBinds();

// The first two alternatives of a value, trimmed, by position: ",Q" is an
// empty first key and Q. A missing one is empty.
std::array<std::string, 2> SplitKeyBind(std::string_view value);
// A value from two alternatives. An empty first one stays as a leading comma,
// so the second keeps its place; the game skips empty alternatives.
std::string JoinKeyBind(std::string_view first, std::string_view second);

// Keys the game keeps for itself: F1 (frame rate overlay), F3, F4, F7 and
// Backtick (the SDK's overlays).
bool IsReservedKey(std::string_view name);
