// The stage of the game's Options screen, shared by the pages drawn on it
// (GameStylePanel, GameStyleControls): the movie's 1280 x 720 scaled to the
// game's image, its paper and curtains, its button prompts and the offset
// copies it outlines its text with.
#pragma once
#include "controls/glyphs.h"
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
namespace Rml { class ElementDocument; }
namespace narutorise::menu::game_stage {
// The movie's outlines are copies of the text under it, offset in stage units:
// labels and the title over four copies, the prompts over four closer ones,
// the values over a single shadow.
using Offsets = std::vector<std::array<float, 2>>;
extern const Offsets kOutline, kPromptOutline, kValueShadow;
inline constexpr uint32_t kWhite = 0xFFFFFF, kInk = 0x000000, kOverOutline = 0xA30000;
// The copies as RmlUi shadows at px pixels per stage unit.
std::string Copies(const Offsets& offsets, float px, uint32_t rgb);
// Fits the document's stage to the game's image in the window and lays its
// paper and curtains out. One rem becomes a stage unit. Returns the dp per
// stage unit.
float Place(Rml::ElementDocument& document, int width, int height, float ratio);
// A prompt: the game's button image on an Xbox pad, the family's glyph on
// another pad, or the key's name with the keyboard (SetPrompts fills it).
std::string Prompt(std::string_view id, std::string_view image, std::string_view attributes = {});
void SetPrompts(Rml::ElementDocument& document, controls::Glyphs glyphs);
}  // namespace narutorise::menu::game_stage
