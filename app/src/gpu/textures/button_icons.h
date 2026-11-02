// The game's button icons drawn as player one's device (controls/glyphs.h).
// The game's movies show Xbox 360 buttons from small DXT5 images; each one is
// recognized by the FNV-1a 64 of its first level (button_icons.inc,
// tools/input-prompts/icon-manifest.py) and its levels are replaced, at the
// same size and format, by the Kenney glyph of the physical input the player
// uses for that guest input. A texture keeps its place, size and the movie's
// animation: only its texels change.
#pragma once
#include "gpu/backend/contracts.h"
#include <cstdint>

namespace gpu::button_icons {
// The icon of a texture just read (levels in t.data); -1 for none.
int32_t Identify(const DrawTexture& t);
// Replaces t.data with the icon's glyph for the current device and profile.
// False keeps the game's own image (a glyph missing from the atlas).
bool Replace(int32_t icon, DrawTexture& t);
// Changes with the device, the settings' choice and the published profile:
// an icon read at another revision is read and replaced again.
uint64_t Revision();
}  // namespace gpu::button_icons
