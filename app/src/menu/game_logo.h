// Optional prelaunch artwork, read from the user's game, never embedded.
#pragma once

#include "menu/game_assets.h"

namespace narutorise::menu::game_logo {
using Image = game_assets::Image;
using Read = game_assets::Read;
// Bounded FRSA lookup, zlib and DDS BC3 decoding. Empty means use the text title.
std::optional<Image> ReadLogo(uint64_t file_size, const Read& read);
// Called only for a game root already accepted by game_image::Identify.
std::optional<Image> Load(const std::filesystem::path& game_root, bool disc_image);
}  // namespace narutorise::menu::game_logo
