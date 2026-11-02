#include "menu/prompts.h"
#include <RmlUi/Core/StringUtilities.h>
#include <array>
#include <utility>

namespace narutorise::menu {
namespace {
struct Action {
  std::string_view name;
  std::array<std::string_view, 2> pad, key;  // atlas names after the family prefix
};
// pad_input.cpp: South confirms, East goes back, West clears, the shoulders
// change tabs, the D-pad or the left stick moves; Enter, Escape, Delete, Page
// Up/Down and the arrows on the keyboard.
constexpr Action kActions[] = {
    {"confirm", {"south"}, {"return"}},
    {"back", {"east"}, {"escape"}},
    {"clear", {"west"}, {"delete"}},
    {"tabs", {"lb", "rb"}, {"pageup", "pagedown"}},
    {"adjust", {"dpad-horizontal"}, {"arrowshorizontal"}},
    {"navigate", {"dpad"}, {"arrows"}},
    {"preview", {"lstick", "rstick"}, {}},
};

std::string ActionGlyphs(const Action& action, controls::Glyphs glyphs) {
  const bool keyboard = glyphs == controls::Glyphs::Keyboard;
  std::string out;
  for (const auto& part : keyboard ? action.key : action.pad)
    if (!part.empty()) out += GlyphRml(std::string(controls::GlyphPrefix(glyphs)) + "-" + std::string(part));
  return out;
}
}  // namespace

std::string GlyphRml(std::string_view sprite) {
  // Keys carry their names inside: they need more room than a pad's buttons.
  const char* kind = sprite.starts_with("key-") ? "glyph key" : "glyph";
  return std::string("<img class='") + kind + "' sprite='" + std::string(sprite) + "'/>";
}

std::string PromptRml(std::string_view text, controls::Glyphs glyphs) {
  std::string out;
  while (!text.empty()) {
    const size_t open = text.find('{');
    const size_t close = open == std::string_view::npos ? open : text.find('}', open);
    if (close == std::string_view::npos) {
      out += Rml::StringUtilities::EncodeRml(std::string(text));
      break;
    }
    out += Rml::StringUtilities::EncodeRml(std::string(text.substr(0, open)));
    const auto name = text.substr(open + 1, close - open - 1);
    bool known = false;
    for (const auto& action : kActions)
      if (action.name == name) {
        out += ActionGlyphs(action, glyphs);
        known = true;
      }
    if (!known) out += Rml::StringUtilities::EncodeRml(std::string(text.substr(open, close - open + 1)));
    text.remove_prefix(close + 1);
  }
  return out;
}
}  // namespace narutorise::menu
