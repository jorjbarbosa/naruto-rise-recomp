// The menus' button hints as RML: each {action} of a hint text becomes the
// glyph of the device in use (app/ui/prompts.rcss), the rest stays text.
#pragma once
#include "controls/glyphs.h"
#include <string>
#include <string_view>

namespace narutorise::menu {
// Actions: {confirm}, {back}, {clear}, {tabs}, {adjust}, {navigate} and
// {preview}. The menus read the same physical buttons on every controller
// (pad_input.cpp): only the family changes the glyph.
std::string PromptRml(std::string_view text, controls::Glyphs glyphs);
// The glyph of one atlas name ("xbox-south", "key-return").
std::string GlyphRml(std::string_view sprite);
}  // namespace narutorise::menu
