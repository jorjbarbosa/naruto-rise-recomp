// Optional prelaunch artwork, read from the user's game, never embedded.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace narutorise::menu::game_logo {
struct Image {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;  // Straight alpha; no GPU objects on the worker.
};
using Read = std::function<bool(uint64_t offset, std::span<uint8_t> bytes)>;
// Bounded FRSA lookup, zlib and DDS BC3 decoding. Empty means use the text title.
std::optional<Image> ReadLogo(uint64_t file_size, const Read& read);
// Called only for a game root already accepted by game_image::Identify.
std::optional<Image> Load(const std::filesystem::path& game_root, bool disc_image);
}  // namespace narutorise::menu::game_logo
