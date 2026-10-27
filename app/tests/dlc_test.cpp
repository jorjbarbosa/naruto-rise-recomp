// All content operations must honor the runtime's root, without touching Documents.
#include "menu/content_manager.h"
#include <rex/filesystem/devices/stfs_xbox.h>
#include <rex/system/xam/content_manager.h>
#include <algorithm>
#include <fstream>
#include <iostream>

namespace {
constexpr char kPackage[] = "AEFD8711E05CF01634F0053A5AB2E3D2520E1F0A55";
constexpr char kLongPackage[] = "123456789012345678901234567890123456789012-overflow";
constexpr char kShortPackage[] = "123456789012345678901234567890123456789012";

void WritePackage(const std::filesystem::path& path, std::u16string_view name) {
  rex::filesystem::StfsHeader header{};
  header.header.magic = rex::filesystem::XContentPackageType::kLive;
  header.metadata.set_display_name(rex::system::XLanguage::kEnglish, name);
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(&header), sizeof(header));
}

void WriteInstalledHeader(const std::filesystem::path& root, const std::string& filename,
                          std::u16string_view name, bool complete = true) {
  const auto headers = root.parent_path() / "Headers/00000002";
  std::filesystem::create_directories(headers);
  rex::system::xam::XCONTENT_AGGREGATE_DATA header{};
  header.content_type = rex::system::XContentType::kMarketplaceContent;
  header.title_id = 0x555307E5;
  header.set_file_name(filename);
  header.set_display_name(name);
  std::ofstream(headers / (filename + ".header"), std::ios::binary)
      .write(reinterpret_cast<const char*>(&header), complete ? sizeof(header) : 264);
}

bool CheckInstalled(const std::filesystem::path& product) {
  const auto installed = launcher_content::ListInstalledDlc();
  const auto named = [&](const char* filename, const char* name) {
    return std::any_of(installed.begin(), installed.end(), [&](const auto& item) {
      return item.path.filename() == filename && item.name == name;
    });
  };
  const auto pending = launcher_content::ListPendingDlc(product / "dlc");
  return installed.size() == 3 && named(kPackage, "Choji & Temari") &&
         named(kShortPackage, "Vozes japonesas \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e \xf0\x9f\x8e\xae") &&
         pending.size() == 1 && pending[0].name == "addon";
}
}

int main(int argc, char** argv) {
  if (argc != 2 && argc != 3) return 2;
  const auto base = std::filesystem::absolute(argv[1]);
  launcher_content::SetUserDataRoot(base / "user");
  if (argc == 3) {
    if (std::string(argv[2]) != "--reopened" || !CheckInstalled(base / "product")) return 16;
    std::cout << "PASS: names and installed/pending state survive a new process\n";
    return 0;
  }
  std::filesystem::create_directories(base / "loose/addon");
  const auto expected = base / "user/0000000000000000/555307E5/00000002";
  if (launcher_content::GetDlcRoot() != expected) return 3;
  std::string error;
  if (!launcher_content::EnsureDlcFolders(error)) return 4;
  std::ofstream(base / "loose/addon/content.bin") << "test content";
  if (launcher_content::InstallDlc(base / "loose", base / "product/dlc", error) != 0) return 5;
  if (!std::filesystem::exists(expected / "loose/addon/content.bin")) return 6;
  const auto installed = launcher_content::ListInstalledDlc();
  if (installed.size() != 1 || installed[0].path != expected / "loose") return 7;
  std::filesystem::create_directories(base / "packages");
  std::string header(0x344, '\0'); header.replace(0, 4, "LIVE");
  std::ofstream(base / "packages/addon", std::ios::binary).write(header.data(), header.size());
  WritePackage(base / "packages" / kPackage, u"Choji & Temari");
  WritePackage(base / "packages" / kLongPackage, u"Vozes japonesas \u65e5\u672c\u8a9e \U0001f3ae");
  const auto packages = launcher_content::ScanStfsPackages(base / "packages");
  if (packages.size() != 3 || std::none_of(packages.begin(), packages.end(), [](const auto& item) {
        return item.path.filename() == kPackage && item.name == "Choji & Temari";
      })) return 9;
  if (launcher_content::InstallDlc(base / "packages", base / "product/dlc", error) != 3 ||
      !std::filesystem::exists(base / "product/dlc/addon")) return 8;
  const auto staged = base / "product/dlc";
  if (launcher_content::InstallDlc(base / "packages", staged, error) != 3) return 17;
  header[4] = 'X';
  std::ofstream(base / "packages/addon", std::ios::binary).write(header.data(), header.size());
  if (launcher_content::InstallDlc(base / "packages", staged, error) != -1 ||
      error.find("conflict") == std::string::npos ||
      std::filesystem::file_size(staged / "addon") != header.size()) return 18;
  error.clear();
  if (launcher_content::InstallDlc(base / "packages", {}, error) != -1) return 19;
  error.clear();
  if (launcher_content::ListPendingDlc(staged).size() != 3) return 10;
  // An extracted folder alone, or a truncated header, must remain pending.
  std::filesystem::create_directories(expected / kPackage);
  std::ofstream(expected / kPackage / "content.bin") << "extracted content";
  if (launcher_content::ListPendingDlc(staged).size() != 3) return 11;
  WriteInstalledHeader(expected, kPackage, u"Choji & Temari", false);
  if (launcher_content::ListPendingDlc(staged).size() != 3) return 12;
  WriteInstalledHeader(expected, kPackage, u"Choji & Temari");
  // Matching display names do not identify packages: addon has a header but
  // no extracted folder and must still be offered for installation.
  WriteInstalledHeader(expected, "addon", u"Choji & Temari");
  if (launcher_content::ListPendingDlc(staged).size() != 2) return 13;
  std::filesystem::create_directories(expected / kShortPackage);
  std::ofstream(expected / kShortPackage / "content.bin") << "extracted voices";
  WriteInstalledHeader(expected, kShortPackage, u"Vozes japonesas \u65e5\u672c\u8a9e \U0001f3ae");
  if (!CheckInstalled(base / "product")) return 14;
  launcher_content::SetUserDataRoot(base / "other-user");
  if (launcher_content::ListPendingDlc(staged).size() != 3) return 15;
  launcher_content::SetUserDataRoot(base / "user");
  const auto mirror_source = base / "mirror source", mirror_target = base / "mirror target";
  std::filesystem::create_directories(mirror_source);
  std::ofstream(mirror_source / "loose") << "installed bytes";
  WritePackage(mirror_source / "container", u"Do not mirror STFS");
  if (!launcher_content::MirrorDlcTree(mirror_source, mirror_target, error) ||
      !std::filesystem::exists(mirror_target / "loose") || std::filesystem::exists(mirror_target / "container")) return 20;
  std::ofstream(mirror_source / "loose") << "newer divergent bytes";
  std::filesystem::last_write_time(mirror_source / "loose", std::filesystem::file_time_type::clock::now() + std::chrono::hours(1));
  if (launcher_content::MirrorDlcTree(mirror_source, mirror_target, error)) return 21;
  std::ifstream preserved(mirror_target / "loose");
  const std::string bytes{std::istreambuf_iterator<char>(preserved), std::istreambuf_iterator<char>()};
  if (bytes != "installed bytes") return 22;
  // Conflict diagnostics must preserve paths outside the Windows code page.
  const auto unicode = base / std::filesystem::path(u"DLC \u65e5\u672c\u8a9e");
  const auto unicode_source = unicode / "source", unicode_target = unicode / "target";
  std::filesystem::create_directories(unicode_source);
  std::filesystem::create_directories(unicode_target);
  std::ofstream(unicode_source / "content") << "original source";
  std::ofstream(unicode_target / "content") << "original destination";
  error.clear();
  if (launcher_content::MirrorDlcTree(unicode_source, unicode_target, error) ||
      error.find("conflict") == std::string::npos ||
      error.find("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e") == std::string::npos) return 23;
  std::ifstream unicode_preserved(unicode_target / "content");
  if (std::string(std::istreambuf_iterator<char>(unicode_preserved), {}) != "original destination") return 24;
  error.clear();
  if (launcher_content::InstallDlc(unicode / "missing", staged, error) != -1 ||
      error.find("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e") == std::string::npos) return 25;
  std::cout << "PASS: effective user root, loose installation, package names, pending/installed state, long IDs, UTF-16 names\n";
}
