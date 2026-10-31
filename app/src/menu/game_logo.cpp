#include "menu/game_logo.h"

namespace narutorise::menu::game_logo {
namespace {
constexpr std::array<std::string_view, 1> kLogo = {"6e0003ce_I15"};
std::optional<Image> First(std::optional<std::vector<Image>> images) {
  if (!images || images->size() != 1) return {};
  return std::move(images->front());
}
}  // namespace

std::optional<Image> ReadLogo(uint64_t file_size, const Read& read) {
  return First(game_assets::ReadImages(file_size, read, kLogo));
}

std::optional<Image> Load(const std::filesystem::path& game_root, bool disc_image) {
  return First(game_assets::Load(game_root, disc_image, kLogo));
}
}  // namespace narutorise::menu::game_logo
