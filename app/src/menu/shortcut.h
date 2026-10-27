// Shell links retain explicit session overrides; persisted selection stays authoritative.
#pragma once
#include <filesystem>
#include <vector>
#include <string>
#include "data_paths.h"
namespace narutorise::menu {
std::filesystem::path DesktopShortcut();
bool WriteShortcut(const std::filesystem::path& link, const std::filesystem::path& executable,
                   const data::DataPaths& paths, const std::vector<std::string>& extra_arguments = {});
}  // namespace narutorise::menu
