#include "content_manager.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <shlobj.h>
#endif

namespace launcher_content {

namespace {

constexpr char kTitleId[] = "555307E5";
constexpr char kMarketplaceContentType[] = "00000002";
constexpr char kCommonXuid[] = "0000000000000000";

bool CopyRecursive(const std::filesystem::path& source,
                   const std::filesystem::path& destination,
                   std::string& error) {
  std::error_code ec;
  std::filesystem::create_directories(destination, ec);
  if (ec) {
    error = "Failed to create destination: " + destination.string();
    return false;
  }

  for (const auto& entry :
       std::filesystem::directory_iterator(source, std::filesystem::directory_options::skip_permission_denied, ec)) {
    if (ec) continue;
    const auto dest_path = destination / entry.path().filename();
    if (entry.is_directory(ec)) {
      if (!CopyRecursive(entry.path(), dest_path, error)) return false;
    } else {
      std::filesystem::copy_file(entry.path(), dest_path,
                                 std::filesystem::copy_options::overwrite_existing, ec);
      if (ec) {
        error = "Failed to copy " + entry.path().string() + ": " + ec.message();
        return false;
      }
    }
  }
  return true;
}

}  // namespace

std::filesystem::path GetUserDataRoot() {
#if defined(_WIN32)
  wchar_t path[MAX_PATH];
  if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, path))) {
    return std::filesystem::path(path) / "narutorise";
  }
#endif
  // Fallback to current path / narutorise (should not happen on Windows).
  return std::filesystem::current_path() / "narutorise";
}

std::filesystem::path GetDlcRoot() {
  return GetUserDataRoot() / kCommonXuid / kTitleId / kMarketplaceContentType;
}

bool EnsureDlcFolders(std::string& error) {
  std::error_code ec;
  std::filesystem::create_directories(GetDlcRoot(), ec);
  if (ec) {
    error = "Failed to create DLC folders: " + ec.message();
    return false;
  }
  return true;
}

bool InstallLooseDlc(const std::filesystem::path& source, std::string& error) {
  if (!std::filesystem::is_directory(source)) {
    error = "Source is not a folder: " + source.string();
    return false;
  }

  std::string ignored;
  if (!EnsureDlcFolders(ignored)) {
    error = ignored;
    return false;
  }

  std::error_code ec;

  // Some downloads already contain the full Xbox 360 content layout:
  //   <selected>/555307E5/00000002/<dlc_name>/
  // In that case, copy each <dlc_name> directly into the DLC root. If the
  // content files are loose inside 00000002 (no subfolder), use the selected
  // folder name as the DLC package name.
  const auto xbox_layout = source / kTitleId / kMarketplaceContentType;
  if (std::filesystem::is_directory(xbox_layout, ec)) {
    bool any_subfolder = false;
    for (const auto& entry : std::filesystem::directory_iterator(xbox_layout, ec)) {
      if (!entry.is_directory(ec)) continue;
      any_subfolder = true;
      const auto dest = GetDlcRoot() / entry.path().filename();
      if (!CopyRecursive(entry.path(), dest, error)) return false;
    }
    if (!any_subfolder) {
      const auto dest = GetDlcRoot() / source.filename();
      return CopyRecursive(xbox_layout, dest, error);
    }
    return true;
  }

  const auto dest = GetDlcRoot() / source.filename();
  return CopyRecursive(source, dest, error);
}

int InstallDlc(const std::filesystem::path& source,
               const std::filesystem::path& exe_dir,
               std::string& error) {
  if (!std::filesystem::is_directory(source)) {
    error = "Source is not a folder: " + source.string();
    return -1;
  }

  // STFS packages must be installed by the game itself: only
  // ContentManager::InstallContent (which lives in the game process, next to
  // the emulated kernel) extracts them and writes the content header with the
  // license from the package. Stage them into the dlc/ folder next to
  // narutorise.exe; the game picks them up on the next launch and installs
  // them once.
  const auto packages = ScanStfsPackages(source);
  if (!packages.empty()) {
    if (exe_dir.empty()) {
      error = "Game executable folder not set.";
      return -1;
    }
    const auto staging_root = exe_dir / "dlc";
    std::error_code ec;
    std::filesystem::create_directories(staging_root, ec);
    if (ec) {
      error = "Failed to create " + staging_root.string() + ": " + ec.message();
      return -1;
    }
    for (const auto& package : packages) {
      const auto dest = staging_root / package.path.filename();
      std::filesystem::copy_file(package.path, dest,
                                 std::filesystem::copy_options::overwrite_existing,
                                 ec);
      if (ec) {
        error = "Failed to copy " + package.path.string() + ": " + ec.message();
        return -1;
      }
    }
    return static_cast<int>(packages.size());
  }

  // No STFS packages: treat the selection as already-extracted (loose)
  // content and copy it directly into the user data tree.
  return InstallLooseDlc(source, error) ? 0 : -1;
}

std::vector<ContentInfo> ListInstalledDlc() {
  std::vector<ContentInfo> result;
  const auto root = GetDlcRoot();
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec)) return result;

  for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
    if (entry.is_directory(ec)) {
      result.push_back({entry.path().filename().string(), entry.path(), false});
    }
  }
  std::sort(result.begin(), result.end(),
            [](const ContentInfo& a, const ContentInfo& b) { return a.name < b.name; });
  return result;
}

bool IsStfsPackage(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) return false;
  if (std::filesystem::file_size(path, ec) < 0x344) return false;

  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  std::uint32_t magic = 0;
  file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
  if (!file) return false;

  // Packages start with the ASCII magic 'CON ', 'LIVE' or 'PIRS'. Those
  // bytes are big-endian in the file, so a native (little-endian) uint32
  // read yields the byte-swapped values below.
  return magic == 0x204E4F43u || magic == 0x4556494Cu || magic == 0x53524950u;
}

std::vector<ContentInfo> ScanStfsPackages(const std::filesystem::path& folder) {
  std::vector<ContentInfo> result;
  std::error_code ec;
  if (!std::filesystem::is_directory(folder, ec)) return result;

  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(folder, std::filesystem::directory_options::skip_permission_denied, ec)) {
    if (entry.is_regular_file(ec) && IsStfsPackage(entry.path())) {
      result.push_back({entry.path().filename().string(), entry.path(), true});
    }
  }
  std::sort(result.begin(), result.end(),
            [](const ContentInfo& a, const ContentInfo& b) { return a.name < b.name; });
  return result;
}

}  // namespace launcher_content
