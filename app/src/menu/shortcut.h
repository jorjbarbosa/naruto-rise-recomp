// Shell links target the game directly and retain its effective user-data root.
#pragma once
#include <filesystem>
namespace narutorise::menu {
std::filesystem::path DesktopShortcut();
bool WriteShortcut(const std::filesystem::path& link, const std::filesystem::path& executable,
                   const std::filesystem::path& user_root);
}  // namespace narutorise::menu
