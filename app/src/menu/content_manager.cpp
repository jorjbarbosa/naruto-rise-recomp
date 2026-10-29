#include "content_manager.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <vector>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <iterator>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/system/xam/content_manager.h>

#if defined(_WIN32)
#include <windows.h>
#include <shlobj.h>
#endif

namespace launcher_content {

namespace {
std::mutex root_mutex;
std::filesystem::path root_override;

constexpr char kTitleId[] = "555307E5";
constexpr char kMarketplaceContentType[] = "00000002";
constexpr char kCommonXuid[] = "0000000000000000";

bool CopyFile(const std::filesystem::path& source,
              const std::filesystem::path& destination, std::string& error) {
  std::error_code ec;
  if (std::filesystem::is_symlink(source, ec) || std::filesystem::is_symlink(destination, ec)) {
    error = "DLC copy refuses symbolic links."; return false;
  }
  if (std::filesystem::exists(destination, ec)) {
    std::ifstream a(source, std::ios::binary), b(destination, std::ios::binary);
    if (a && b && std::filesystem::file_size(source) == std::filesystem::file_size(destination) &&
        std::equal(std::istreambuf_iterator<char>(a), std::istreambuf_iterator<char>(),
                   std::istreambuf_iterator<char>(b), std::istreambuf_iterator<char>()) &&
        !a.bad() && !b.bad()) return true;
    error = "DLC conflict: " + rex::path_to_utf8(source) + " / " + rex::path_to_utf8(destination) +
            ". Both copies preserved."; return false;
  }
  if (std::filesystem::copy_file(source, destination, std::filesystem::copy_options::none, ec)) return true;
  error = "Failed to copy DLC: " + ec.message(); return false;
}

bool CopyRecursive(const std::filesystem::path& source,
                   const std::filesystem::path& destination,
                   std::string& error, bool skip_packages = false) {
  std::error_code ec;
  std::filesystem::create_directories(destination, ec);
  if (ec) {
    error = "Failed to create destination: " + rex::path_to_utf8(destination);
    return false;
  }

  for (const auto& entry :
       std::filesystem::directory_iterator(source, std::filesystem::directory_options::skip_permission_denied, ec)) {
    if (ec) continue;
    const auto dest_path = destination / entry.path().filename();
    if (entry.is_symlink(ec)) { error = "DLC copy refuses symbolic links."; return false; }
    if (entry.is_directory(ec)) {
      if (!CopyRecursive(entry.path(), dest_path, error, skip_packages)) return false;
    } else {
      if (skip_packages && IsStfsPackage(entry.path())) continue;
      if (!CopyFile(entry.path(), dest_path, error)) return false;
    }
  }
  return true;
}

void AppendUtf8(std::string& out, char32_t c) {
  if (c < 0x80) {
    out += static_cast<char>(c);
  } else if (c < 0x800) {
    out += static_cast<char>(0xC0 | (c >> 6));
    out += static_cast<char>(0x80 | (c & 0x3F));
  } else if (c < 0x10000) {
    out += static_cast<char>(0xE0 | (c >> 12));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (c & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (c >> 18));
    out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (c & 0x3F));
  }
}

// Both STFS metadata and installed headers store bounded big-endian UTF-16.
std::string DisplayName(const void* data, size_t units) {
  const auto* raw = static_cast<const unsigned char*>(data);
  std::string name;
  for (size_t i = 0; i < units; ++i) {
    char32_t unit = (raw[i * 2] << 8) | raw[i * 2 + 1];
    if (unit == 0) break;
    if (unit >= 0xD800 && unit < 0xDC00 && i + 1 < units) {
      const char32_t low = (raw[(i + 1) * 2] << 8) | raw[(i + 1) * 2 + 1];
      if (low >= 0xDC00 && low < 0xE000) {
        unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
        ++i;
      }
    }
    if (unit >= 0xD800 && unit < 0xE000) unit = 0xFFFD;
    AppendUtf8(name, unit);
  }
  return name;
}

std::optional<rex::system::xam::XCONTENT_AGGREGATE_DATA> ReadContentHeader(
    const std::filesystem::path& path) {
  rex::system::xam::XCONTENT_AGGREGATE_DATA header{};
  std::ifstream file(path, std::ios::binary);
  if (!file.read(reinterpret_cast<char*>(&header), sizeof(header))) return std::nullopt;
  return header;
}

std::filesystem::path ContentHeaderPath(const std::filesystem::path& root,
                                      const std::filesystem::path& name) {
  auto header = name;
  header += ".header";
  return root.parent_path() / "Headers" / kMarketplaceContentType / header;
}

std::string PackageDisplayName(const std::filesystem::path& path) {
  const auto header = rex::filesystem::StfsContainerDevice::ReadPackageHeader(path);
  if (!header) return {};
  return DisplayName(header->metadata.display_name_raw.uint[0], 128);
}

}  // namespace

void SetUserDataRoot(std::filesystem::path root) {
  std::lock_guard lock(root_mutex);
  root_override = std::move(root);
}
std::filesystem::path GetUserDataRoot() {
  {
    std::lock_guard lock(root_mutex);
    if (!root_override.empty()) return root_override;
  }
  throw std::logic_error("Content root was not initialized by the application");
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
    error = "Source is not a folder: " + rex::path_to_utf8(source);
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
               const std::filesystem::path& staging_root,
               std::string& error) {
  if (!std::filesystem::is_directory(source)) {
    error = "Source is not a folder: " + rex::path_to_utf8(source);
    return -1;
  }

  // STFS packages must be installed by the game itself: only
  // ContentManager::InstallContent (which lives in the game process, next to
  // the emulated kernel) extracts them and writes the content header with the
  // license from the package. Stage into the selected data folder;
  // the game picks them up on the next launch and installs
  // them once.
  const auto packages = ScanStfsPackages(source);
  if (!packages.empty()) {
    if (staging_root.empty()) {
      error = "DLC package staging is disabled in the active configuration.";
      return -1;
    }
    std::error_code ec;
    std::filesystem::create_directories(staging_root, ec);
    if (ec) {
      error = "Failed to create " + rex::path_to_utf8(staging_root) + ": " + ec.message();
      return -1;
    }
    for (const auto& package : packages) {
      const auto dest = staging_root / package.path.filename();
      if (!CopyFile(package.path, dest, error)) return -1;
    }
    return static_cast<int>(packages.size());
  }

  // No STFS packages: treat the selection as already-extracted (loose)
  // content and copy it directly into the user data tree.
  return InstallLooseDlc(source, error) ? 0 : -1;
}

bool MirrorDlcTree(const std::filesystem::path& source, const std::filesystem::path& destination,
                   std::string& error) {
  try { return CopyRecursive(source, destination, error, true); }
  catch (const std::exception& failure) { error = failure.what(); return false; }
}

std::vector<ContentInfo> ListInstalledDlc() {
  std::vector<ContentInfo> result;
  const auto root = GetDlcRoot();
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec)) return result;

  for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
    if (entry.is_directory(ec)) {
      const auto header = ReadContentHeader(ContentHeaderPath(root, entry.path().filename()));
      const auto utf8 = entry.path().filename().u8string();
      const std::string folder(utf8.begin(), utf8.end());
      std::string name = header ? DisplayName(header->display_name_raw.uint, 128) : std::string{};
      result.push_back({name.empty() ? folder : std::move(name), entry.path(), false});
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
      std::string name = PackageDisplayName(entry.path());
      const auto utf8 = entry.path().filename().u8string();
      if (name.empty()) name.assign(utf8.begin(), utf8.end());
      result.push_back({std::move(name), entry.path(), true});
    }
  }
  std::sort(result.begin(), result.end(),
            [](const ContentInfo& a, const ContentInfo& b) { return a.name < b.name; });
  return result;
}

std::vector<ContentInfo> ListPendingDlc(const std::filesystem::path& folder) {
  auto packages = ScanStfsPackages(folder);
  const auto root = GetDlcRoot();
  std::erase_if(packages, [&](const ContentInfo& package) {
    // XCONTENT_DATA limits filenames to 42 UTF-8 bytes. Startup also repairs
    // legacy long names without splitting a UTF-8 sequence.
    auto name = package.path.filename().u8string();
    if (name.size() > 42) {
      size_t length = 42;
      while (length && (static_cast<uint8_t>(name[length]) & 0xC0) == 0x80) --length;
      name.resize(length);
    }
    const std::filesystem::path installed_name(name);
    std::error_code ec;
    return std::filesystem::is_directory(root / installed_name, ec) &&
           ReadContentHeader(ContentHeaderPath(root, installed_name)).has_value();
  });
  return packages;
}

}  // namespace launcher_content
