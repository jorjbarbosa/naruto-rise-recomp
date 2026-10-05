#pragma once

#include <filesystem>
#include <string>

namespace game_paths {

// "game" is the published layout. Legacy directories are read-only fallbacks;
// selecting or discovering them never renames or removes the user's files.
inline std::filesystem::path Find(const std::filesystem::path& base_dir,
                                  const std::string& custom_path) {
  namespace fs = std::filesystem;
  const fs::path candidates[] = {
      fs::u8path(custom_path),
      base_dir / "game",
      base_dir / "game_root",
      base_dir / "../../../../game",
      base_dir / "../../../../game_root",
      base_dir / "../../../game",
      base_dir / "../../../game_root",
  };
  for (const auto& candidate : candidates) {
    if (candidate.empty()) continue;
    std::error_code ec;
    if (fs::is_regular_file(candidate / "default.xex", ec)) {
      fs::path canonical = fs::canonical(candidate, ec);
      if (!ec) return canonical;
    }
  }
  return {};
}

}  // namespace game_paths
