#pragma once

#include <filesystem>

// Shared ImGui theme (dark slate + Naruto Chakra orange), used by both the
// launcher and the installer wizard for a consistent visual identity.
namespace launcher_theme {

void Apply();
void LoadFonts(const std::filesystem::path& base_dir);

}  // namespace launcher_theme
