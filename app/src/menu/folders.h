#pragma once

#include <filesystem>
#include <string>

namespace launcher_ui {

// A path as UTF-8 text, for display.
std::string PathText(const std::filesystem::path& path);

// Opens a folder in the system file manager.
void OpenFolder(const std::filesystem::path& folder);

}  // namespace launcher_ui
