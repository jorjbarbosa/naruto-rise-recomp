// Artwork of the game's menus, read from the user's game, never embedded.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace narutorise::menu::game_assets {
struct Image {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;  // Straight alpha; no GPU objects on the worker.
};
using Read = std::function<bool(uint64_t offset, std::span<uint8_t> bytes)>;
// Naruto_clip.SRA records of the in-game settings menu: the Options screen's
// art (9e002bac), the main menu's buttons and star (6e0003c4) and the paper
// with the spiral (790006c2). Load returns them in this order.
inline constexpr std::array<std::string_view, 13> kMenuImages = {
    "790006c2_I2",    // paper with the spiral (BC1)
    "9e002bac_I105",  // curtain
    "9e002bac_I102",  // curtain edge
    "9e002bac_I10F",  // ink splash closing the title band
    "9e002bac_I120",  // dark brush stroke
    "9e002bac_I123",  // blue brush stroke
    "9e002bac_I128",  // A button
    "9e002bac_I131",  // B button
    "9e002bac_IF4",   // cursor cloud
    "9e002bac_IF6",   // white cloud
    "6e0003c4_I9",    // LB
    "6e0003c4_IB",    // RB
    "6e0003c4_I90",   // restart star
};
// The documents' names for them, as "game/<name>" image sources.
inline constexpr std::array<std::string_view, kMenuImages.size()> kMenuImageKeys = {
    "paper", "curtain", "curtain-edge", "splash", "brush-dark", "brush-blue", "button-a", "button-b",
    "cloud", "cloud-white", "lb", "rb", "star"};
// Bounded FRSA lookup of every name, zlib and DDS BC1/BC3 decoding. All or
// nothing: empty when a name is missing, ambiguous or malformed.
std::optional<std::vector<Image>> ReadImages(uint64_t file_size, const Read& read,
                                             std::span<const std::string_view> names);
// Naruto_clip.SRA of a game root already accepted by game_image::Identify.
std::optional<std::vector<Image>> Load(const std::filesystem::path& game_root, bool disc_image,
                                       std::span<const std::string_view> names);
}  // namespace narutorise::menu::game_assets
