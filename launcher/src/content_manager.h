#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace launcher_content {

struct ContentInfo {
  std::string name;
  std::filesystem::path path;
  bool is_stfs = false;
};

// Returns the default user data root used by narutorise.exe
// (Documents/narutorise on Windows).
std::filesystem::path GetUserDataRoot();

// Returns the DLC content root for this title:
// <user_data_root>/0000000000000000/555307E5/00000002
std::filesystem::path GetDlcRoot();

// Ensures the DLC folder hierarchy exists.
bool EnsureDlcFolders(std::string& error);

// Copies loose (already extracted) DLC files into the correct content folder.
bool InstallLooseDlc(const std::filesystem::path& source, std::string& error);

// Installs a DLC folder selected by the user. STFS packages found inside are
// staged into the dlc/ folder next to narutorise.exe; the game installs them
// on the next launch via ContentManager::InstallContent (which writes the
// content header with the license from the package itself). Folders with
// already-extracted content fall back to a direct copy into the user data
// tree. Returns the number of staged STFS packages.
int InstallDlc(const std::filesystem::path& source,
               const std::filesystem::path& exe_dir,
               std::string& error);

// Lists installed DLC folders (extracted content in the user data tree).
std::vector<ContentInfo> ListInstalledDlc();

// Checks whether a file looks like an STFS Xbox 360 package.
bool IsStfsPackage(const std::filesystem::path& path);

// Scans a folder and returns STFS packages found.
std::vector<ContentInfo> ScanStfsPackages(const std::filesystem::path& folder);

}  // namespace launcher_content
