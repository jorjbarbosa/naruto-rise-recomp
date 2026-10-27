#include "data_paths.h"

#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace narutorise::data;
namespace {
int checks = 0;
void Check(bool ok, const std::string& message) {
  ++checks;
  if (!ok) throw std::runtime_error(message);
}
template <typename T>
T Value(Result<T> result) {
  Check(static_cast<bool>(result), result.error);
  return std::move(*result.value);
}
std::string Utf8(const fs::path& path) {
  const auto text = path.u8string();
  return {text.begin(), text.end()};
}
fs::path Path(std::string_view text) {
  return fs::path(std::u8string(text.begin(), text.end()));
}
void Write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary);
  stream << text;
  if (!stream) throw std::runtime_error("Cannot write fixture");
}
void Descriptor(const fs::path& file, const std::string& mode,
                const std::string& path = "", const std::string& pending = "") {
  nlohmann::json doc{{"version", 1}, {"mode", mode}};
  if (!path.empty()) doc["path"] = path;
  if (!pending.empty()) doc["pending_source"] = pending;
  Write(file, doc.dump());
}
Environment Fixture(const fs::path& root, Platform platform) {
  Environment env;
  env.platform = platform;
  env.install_dir = root / "installation";
  env.initial_cwd = root / "different cwd";
  env.home_dir = root / "home";
  env.local_app_data = root / "local app data";
  fs::create_directories(env.install_dir);
  fs::create_directories(env.initial_cwd);
  return env;
}

void WindowsPolicy(const fs::path& root) {
  auto env = Fixture(root, Platform::kWindows);
  auto paths = Value(ResolvePaths(env));
  Check(paths.data_root == env.install_dir / "data" &&
            paths.user_data_root == paths.data_root,
        "ZIP defaults to exe/data with the guest tree directly inside");
  Check(paths.cache_root == paths.data_root / "cache" &&
            paths.log_dir == paths.data_root / "logs" &&
            paths.config_path == paths.data_root / "narutorise.toml" &&
            paths.dlc_dir == paths.data_root / "dlc" &&
            paths.mods_dir == paths.data_root / "mods",
        "default mutable layout");
  Check(paths.defaults_path == env.install_dir / "defaults/narutorise.toml" &&
            paths.distribution_dir == env.install_dir &&
            paths.allow_legacy_import,
        "immutable resources and eligible import");
  Check(!fs::exists(paths.data_root) && !fs::exists(paths.user_descriptor),
        "resolution has no writes");
  env.data_root = "";
  Check(Value(ResolvePaths(env)).data_source == Source::kDefault,
        "empty REX_DATA_ROOT keeps default");
  env.data_root.reset();
  const auto cwd = fs::current_path();
  fs::current_path(env.install_dir);
  Overrides relative;
  relative.data_root = "captured cwd";
  const auto after_chdir = ResolvePaths(env, relative);
  fs::current_path(cwd);
  Check(Value(after_chdir).data_root == env.initial_cwd / "captured cwd",
        "relative CLI uses captured cwd after chdir");
  fs::create_directories(paths.data_root);
  auto unchanged = Value(ResolvePaths(env));
  Check(unchanged.data_source == Source::kDefault,
        "data directory existence is not a portable marker");

  Descriptor(paths.user_descriptor, "custom", "chosen user folder");
  paths = Value(ResolvePaths(env));
  Check(paths.data_source == Source::kUserDescriptor &&
            paths.data_root ==
                paths.user_descriptor.parent_path() / "chosen user folder",
        "user descriptor base");
  Descriptor(paths.portable_descriptor, "portable");
  paths = Value(ResolvePaths(env));
  Check(paths.mode == Mode::kPortable &&
            paths.data_source == Source::kPortableDescriptor &&
            paths.data_root == env.install_dir / "data",
        "portable descriptor wins");
  Descriptor(paths.portable_descriptor, "custom", "../selected", "../old root");
  paths = Value(ResolvePaths(env));
  Check(paths.data_root == root / "selected" &&
            paths.pending_source == root / "old root",
        "descriptor relative paths have a stable base");
  Descriptor(paths.portable_descriptor, "default");
  paths = Value(ResolvePaths(env));
  Check(paths.mode == Mode::kDefault &&
            paths.data_root == env.install_dir / "data",
        "explicit default suppresses user selection");
  Write(paths.portable_descriptor, "{invalid");
  auto bad = ResolvePaths(env);
  Check(!bad && bad.error.find("data_location.json") != std::string::npos,
        "invalid descriptor is fatal");
  env.data_root = "session environment";
  paths = Value(ResolvePaths(env));
  Check(paths.data_source == Source::kEnvironment &&
            paths.data_root == env.initial_cwd / "session environment" &&
            !paths.allow_legacy_import && paths.session_override &&
            !paths.pending_source,
        "environment isolates session and bypasses broken descriptor");
  Overrides cli;
  cli.data_root = "session command/../command";
  paths = Value(ResolvePaths(env, cli));
  Check(paths.data_source == Source::kCommandLine &&
            paths.data_root == env.initial_cwd / "command",
        "CLI wins and normalizes relative path");
  cli.data_root = "";
  Check(!ResolvePaths(env, cli), "explicit empty CLI root is an error");
  cli.data_root = std::string("invalid\0root", 12);
  Check(!ResolvePaths(env, cli),
        "embedded NUL cannot silently truncate a path");
  env.data_root.reset();
  for (const auto* invalid :
       {R"({"version":2,"mode":"portable"})",
        R"({"version":true,"mode":"portable"})",
        R"({"version":1,"mode":"unknown"})",
        R"({"version":1,"mode":"custom","path":42})",
        R"({"version":1,"mode":"custom","path":""})",
        R"({"version":1,"mode":"portable","path":"x"})",
        R"({"version":1,"mode":"portable","pending_source":false})", "[]"}) {
    Write(paths.portable_descriptor, invalid);
    Check(!ResolvePaths(env), "malformed descriptor must not fall back");
  }
  Write(paths.portable_descriptor, std::string(65537, ' '));
  Check(!ResolvePaths(env), "oversized descriptor rejected");
  fs::remove(paths.portable_descriptor);
  env.local_app_data.clear();
  Check(!ResolvePaths(env),
        "missing user descriptor base must not silently reset persisted "
        "selection");
  Descriptor(paths.portable_descriptor, "portable");
  Check(static_cast<bool>(ResolvePaths(env)),
        "portable descriptor works without personal discovery");
  fs::remove(paths.portable_descriptor);
  env.local_app_data = root / "local app data";
  fs::create_directory(paths.portable_descriptor);
  Check(!ResolvePaths(env), "directory masquerading as descriptor rejected");
}

void LinuxPolicy(const fs::path& root) {
  auto env = Fixture(root, Platform::kLinux);
  env.appimage = Utf8(root / "distribution with spaces/Naruto.AppImage");
  auto paths = Value(ResolvePaths(env));
  Check(paths.data_root == env.home_dir / ".local/share/narutorise" &&
            paths.cache_root == env.home_dir / ".cache/narutorise",
        "Linux XDG defaults");
  Check(paths.distribution_dir == root / "distribution with spaces" &&
            paths.install_dir == env.install_dir &&
            paths.defaults_path.parent_path() == env.install_dir / "defaults",
        "AppImage resources use mounted install");
  Check(paths.user_descriptor ==
            env.home_dir / ".config/narutorise/data_location.json",
        "stable Linux descriptor");
  for (const auto* variable : {"", "relative/base"}) {
    env.xdg_data_home = env.xdg_cache_home = env.xdg_config_home = variable;
    paths = Value(ResolvePaths(env));
    Check(paths.data_root == env.home_dir / ".local/share/narutorise" &&
              paths.cache_root == env.home_dir / ".cache/narutorise" &&
              paths.user_descriptor ==
                  env.home_dir / ".config/narutorise/data_location.json",
          "empty/relative XDG fallback");
  }
  env.xdg_data_home = Utf8(root / fs::path(u8"dados 日本語"));
  env.xdg_cache_home = Utf8(root / "cache");
  env.xdg_config_home = Utf8(root / "configuration");
  paths = Value(ResolvePaths(env));
  Check(paths.data_root == Path(env.xdg_data_home) / "narutorise" &&
            paths.cache_root == root / "cache/narutorise",
        "absolute XDG and Unicode");
#ifndef _WIN32
  Check(chmod(env.install_dir.c_str(), 0500) == 0,
        "make simulated AppImage installation read-only");
#endif
  auto writable = ValidateWritablePaths(paths);
#ifndef _WIN32
  Check(chmod(env.install_dir.c_str(), 0700) == 0,
        "restore isolated installation permissions");
#endif
  Check(Value(writable), "Linux write validation with read-only installation");
  Check(fs::is_empty(env.install_dir) && !fs::exists(paths.portable_descriptor),
        "no writes to install/distribution");
  Descriptor(paths.portable_descriptor, "portable");
  paths = Value(ResolvePaths(env));
  Check(paths.data_root == paths.distribution_dir / "data" &&
            paths.cache_root == paths.data_root / "cache",
        "explicit portable mode follows the external AppImage");
  Descriptor(paths.portable_descriptor, "custom", Utf8(paths.data_root),
             Utf8(paths.data_root));
  paths = Value(ResolvePaths(env));
  Check(paths.pending_source == paths.data_root,
        "source equals destination represented without a copy");
  fs::remove(paths.portable_descriptor);
  env.home_dir.clear();
  Check(static_cast<bool>(ResolvePaths(env)),
        "absolute XDG works without HOME");
  env.xdg_data_home = env.xdg_cache_home = env.xdg_config_home = "";
  Check(!ResolvePaths(env), "missing home and XDG produces error");
  env.data_root = Utf8(root / "fully isolated");
  env.cache_root = Utf8(root / "isolated cache");
  Check(static_cast<bool>(ResolvePaths(env)),
        "explicit data/cache work without user home");
  env.xdg_config_home = "relative/config";
  Check(static_cast<bool>(ResolvePaths(env)),
        "isolated session ignores unavailable personal descriptor base");
  env.home_dir = root / "Naruto.AppImage.home";
  env.xdg_config_home = Utf8(root / "Naruto.AppImage.config");
  env.data_root.reset();
  env.cache_root.reset();
  paths = Value(ResolvePaths(env));
  Check(paths.data_root == env.home_dir / ".local/share/narutorise" &&
            paths.user_descriptor.parent_path() ==
                Path(env.xdg_config_home) / "narutorise",
        "effective AppImage home/config respected");
}

void StorageOverrides(const fs::path& root) {
  auto env = Fixture(root, Platform::kWindows);
  auto paths = Value(ResolvePaths(env));
  ConfigRoots config;
  config.user_data_root = "old relative saves";
  auto selected = Value(ApplyStorageOverrides(paths, env, {}, config));
  Check(selected.user_data_root == env.install_dir / "old relative saves" &&
            selected.user_source == Source::kConfig &&
            selected.cache_root == selected.user_data_root / "cache",
        "historical TOML guest base and derived cache");
  config.relative_base = root / "historical install";
  selected = Value(ApplyStorageOverrides(paths, env, {}, config));
  Check(selected.user_data_root == config.relative_base / "old relative saves",
        "imported config historical base");
  env.user_data_root = "environment saves";
  selected = Value(ApplyStorageOverrides(paths, env, {}, config));
  Check(selected.user_data_root == env.initial_cwd / "environment saves" &&
            selected.user_source == Source::kEnvironment,
        "environment guest beats TOML");
  Overrides cli;
  cli.user_data_root = Utf8(root / fs::path(u8"saves ação 日本語")) + "/";
  selected = Value(ApplyStorageOverrides(paths, env, cli, config));
  Check(selected.user_data_root.lexically_normal() ==
                Path(*cli.user_data_root).lexically_normal() &&
            selected.user_source == Source::kCommandLine,
        "CLI guest Unicode root preserved");
  config.cache_root = "configured cache";
  selected = Value(ApplyStorageOverrides(paths, env, cli, config));
  Check(selected.cache_root == config.relative_base / "configured cache",
        "configured cache beats derived cache");
  env.cache_root = "environment cache";
  selected = Value(ApplyStorageOverrides(paths, env, cli, config));
  Check(selected.cache_root == env.initial_cwd / "environment cache",
        "environment cache beats config");
  cli.cache_root = "command cache";
  selected = Value(ApplyStorageOverrides(paths, env, cli, config));
  Check(
      selected.cache_root == env.initial_cwd / "command cache" &&
          selected.cache_source == Source::kCommandLine &&
          selected.config_path == paths.config_path &&
          selected.data_root == paths.data_root,
      "cache CLI beats environment and guest never redirects general storage");
}

void WriteFailures(const fs::path& root) {
  auto env = Fixture(root, Platform::kWindows);
  Write(env.install_dir / "data", "existing file must survive");
  const auto paths = Value(ResolvePaths(env));
  auto failed = ValidateWritablePaths(paths);
  Check(!failed &&
            failed.error.find(Utf8(paths.data_root)) != std::string::npos &&
            failed.error.find("--data_root") != std::string::npos,
        "invalid destination actionable error with path");
  Check(fs::file_size(paths.data_root) ==
                std::string("existing file must survive").size() &&
            !fs::exists(env.home_dir),
        "failure preserves file and has no fallback");
  fs::remove(paths.data_root);
  Check(Value(ValidateWritablePaths(paths)), "create writable roots");
  Write(paths.data_root / ".narutorise-write-check-0", "keep");
  Check(Value(ValidateWritablePaths(paths)),
        "exclusive probe skips occupied filename");
  Check(fs::file_size(paths.data_root / ".narutorise-write-check-0") == 4 &&
            !fs::exists(paths.data_root / ".narutorise-write-check-1"),
        "probe cleanup without truncation");
#ifdef _WIN32
  DWORD original_size = 0;
  GetFileSecurityW(paths.data_root.c_str(), DACL_SECURITY_INFORMATION, nullptr,
                   0, &original_size);
  std::vector<char> original(original_size);
  Check(
      GetFileSecurityW(paths.data_root.c_str(), DACL_SECURITY_INFORMATION,
                       reinterpret_cast<PSECURITY_DESCRIPTOR>(original.data()),
                       original_size, &original_size),
      "read isolated folder ACL");
  PSECURITY_DESCRIPTOR denied = nullptr;
  Check(ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(D;;GW;;;WD)(A;;GA;;;WD)", SDDL_REVISION_1, &denied, nullptr),
        "prepare denied-write ACL");
  const bool applied = SetFileSecurityW(paths.data_root.c_str(),
                                        DACL_SECURITY_INFORMATION, denied);
  LocalFree(denied);
  Check(applied, "apply denied-write ACL to isolated fixture");
  failed = ValidateWritablePaths(paths);
  const bool restored =
      SetFileSecurityW(paths.data_root.c_str(), DACL_SECURITY_INFORMATION,
                       reinterpret_cast<PSECURITY_DESCRIPTOR>(original.data()));
  Check(restored && !failed,
        "Windows permission denial is fatal and fixture ACL restored");
#else
  if (geteuid() != 0) {
    chmod(paths.data_root.c_str(), 0500);
    failed = ValidateWritablePaths(paths);
    chmod(paths.data_root.c_str(), 0700);
    Check(!failed, "permission denial does not fall back");
  } else {
    throw std::runtime_error(
        "Run as a non-root user to test permission denial");
  }
  fs::create_symlink(root / "missing descriptor", paths.portable_descriptor);
  Check(!ResolvePaths(env),
        "broken descriptor symlink does not select empty saves");
#endif
}

void Capture(const fs::path& root) {
  std::map<std::string, std::string> variables{
      {"REX_DATA_ROOT", Utf8(root / fs::path(u8"captured 日本語"))}};
  int discoveries = 0;
  auto reader = [&](std::string_view key) -> std::optional<std::string> {
    auto it = variables.find(std::string(key));
    return it == variables.end() ? std::nullopt : std::optional(it->second);
  };
  auto home = [&] {
    ++discoveries;
    return root / "discovered home";
  };
  auto env = Value(CaptureEnvironment(root, reader, home));
  Check(discoveries == 1 && env.home_dir == root / "discovered home" &&
            env.data_root == variables["REX_DATA_ROOT"] &&
            env.initial_cwd == fs::current_path(),
        "missing HOME uses injectable platform discovery");
  for (const auto* invalid : {"", "relative"}) {
    variables["HOME"] = invalid;
    env = Value(CaptureEnvironment(root, reader, home));
    Check(env.home_dir == root / "discovered home",
          "invalid HOME uses discovery");
  }
  variables["HOME"] = Utf8(root / "effective home");
  env = Value(CaptureEnvironment(root, reader, home));
  Check(env.home_dir == root / "effective home" && discoveries == 3,
        "effective absolute HOME wins");
  Check(!CaptureEnvironment("relative install", reader, home),
        "installation base must be absolute");
  // Exercise native discovery as well; resolution remains read-only.
  env = Value(CaptureEnvironment(root));
  Check(env.initial_cwd.is_absolute() && env.home_dir.is_absolute(),
        "native capture returns absolute paths");
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  try {
    const auto root = fs::absolute(Path(argv[1])).lexically_normal();
    if (fs::exists(root))
      throw std::runtime_error("Fixture must be a new isolated directory");
    fs::create_directories(root);
    WindowsPolicy(root / "windows");
    LinuxPolicy(root / "linux");
    StorageOverrides(root / "overrides");
    auto dlc = Value(ResolvePaths(Fixture(root / "dlc", Platform::kWindows)));
    Check(CanonicalPath(root / "missing/deeper") == fs::canonical(root) / "missing/deeper", "missing suffix keeps canonical accessible ancestor");
    Check(ResolveDlcSource(dlc, "", dlc.install_dir).empty(), "empty DLC disables staging");
    Check(ResolveDlcSource(dlc, "dlc", dlc.install_dir) == dlc.dlc_dir, "default DLC follows data root");
    Check(ResolveDlcSource(dlc, "custom", root) == root / "custom", "custom DLC preserves historical base");
    WriteFailures(root / "write failures");
    Capture(root);
    std::cout << "PASS: " << checks
              << " checks; ZIP/AppImage selection, descriptor errors, "
                 "overrides, Unicode, write validation\n";
  } catch (const std::exception& error) {
    std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
    return 1;
  }
}
