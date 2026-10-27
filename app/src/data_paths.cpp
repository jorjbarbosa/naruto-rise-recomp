#include "data_paths.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#else
#include <fcntl.h>
#include <pwd.h>
#include <unistd.h>
#endif

namespace narutorise::data {
namespace {
namespace fs = std::filesystem;

std::string Utf8(const fs::path& path) {
  const auto text = path.u8string();
  return {text.begin(), text.end()};
}

fs::path ParsePath(std::string_view value) {
  if (value.empty() || value.find('\0') != std::string_view::npos)
    throw std::runtime_error("Empty path or embedded NUL in data location");
  return fs::path(std::u8string(value.begin(), value.end()));
}

fs::path Resolve(const fs::path& path, const fs::path& base) {
  // Reject Windows drive-relative/root-relative paths: their interpretation
  // depends on process state rather than the captured base.
  if (!path.is_absolute() && path.has_root_path())
    throw std::runtime_error(
        "Path must be absolute or relative to its documented base: " +
        Utf8(path));
  return (path.is_absolute() ? path : base / path).lexically_normal();
}

fs::path AbsoluteDirectory(const fs::path& path, std::string_view name) {
  if (path.empty() || !path.is_absolute())
    throw std::runtime_error(std::string(name) +
                             " is unavailable or not absolute");
  return path.lexically_normal();
}

fs::path XdgBase(const std::string& value, const Environment& env,
                 const fs::path& suffix) {
  if (!value.empty()) {
    auto path = ParsePath(value);
    if (path.is_absolute()) return path.lexically_normal();
  }
  return AbsoluteDirectory(env.home_dir, "User home") / suffix;
}

fs::path DefaultRoot(const Environment& env) {
  if (env.platform == Platform::kWindows) return env.install_dir / "data";
  return XdgBase(env.xdg_data_home, env, ".local/share") / "narutorise";
}

fs::path DefaultCache(const Environment& env, const DataPaths& paths) {
  if (env.platform == Platform::kWindows || paths.mode == Mode::kPortable)
    return paths.data_root / "cache";
  return XdgBase(env.xdg_cache_home, env, ".cache") / "narutorise";
}

bool DescriptorExists(const fs::path& path) {
  if (path.empty()) return false;
  std::error_code ec;
  auto status = fs::symlink_status(path, ec);
  if (ec == std::errc::no_such_file_or_directory) return false;
  if (ec)
    throw std::runtime_error("Cannot inspect data descriptor " + Utf8(path) +
                             ": " + ec.message());
  return status.type() != fs::file_type::not_found;
}

void ReadDescriptor(DataPaths& paths, const Environment& env,
                    const fs::path& file) {
  try {
    if (!fs::is_regular_file(file) || fs::file_size(file) > 65536)
      throw std::runtime_error("expected a JSON file of at most 64 KiB");
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read file");
    const auto doc = nlohmann::json::parse(input);
    if (!doc.is_object() || !doc.contains("version") ||
        !doc["version"].is_number_integer() || doc["version"] != 1 ||
        !doc.contains("mode") || !doc["mode"].is_string())
      throw std::runtime_error("expected version 1 and a location mode");
    const auto mode = doc["mode"].get<std::string>();
    if (mode == "default") {
      paths.mode = Mode::kDefault;
      paths.data_root = DefaultRoot(env);
    } else if (mode == "portable") {
      paths.mode = Mode::kPortable;
      paths.data_root = paths.distribution_dir / "data";
    } else if (mode == "custom") {
      paths.mode = Mode::kCustom;
      paths.data_root = Resolve(ParsePath(doc.at("path").get<std::string>()),
                                file.parent_path());
    } else {
      throw std::runtime_error("unknown location mode");
    }
    if (mode != "custom" && doc.contains("path"))
      throw std::runtime_error("path is only valid in custom mode");
    if (doc.contains("pending_source"))
      paths.pending_source =
          Resolve(ParsePath(doc["pending_source"].get<std::string>()),
                  file.parent_path());
    paths.selected_descriptor = file;
  } catch (const std::exception& e) {
    throw std::runtime_error("Invalid data descriptor " + Utf8(file) + ": " +
                             e.what());
  }
}

std::optional<std::string> ReadEnvironment(std::string_view name) {
#ifdef _WIN32
  const auto wide_name = ParsePath(name).wstring();
  const auto size = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
  if (!size) return std::nullopt;
  std::wstring value(size, L'\0');
  const auto count =
      GetEnvironmentVariableW(wide_name.c_str(), value.data(), size);
  if (!count || count >= size)
    throw std::runtime_error("Environment changed during path bootstrap");
  value.resize(count);
  return Utf8(fs::path(value));
#else
  const auto key = std::string(name);
  if (const char* value = std::getenv(key.c_str())) return std::string(value);
  return std::nullopt;
#endif
}

#ifdef _WIN32
fs::path KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR value = nullptr;
  if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &value)))
    return {};
  fs::path result(value);
  CoTaskMemFree(value);
  return result;
}
#endif

fs::path DiscoverHome() {
#ifdef _WIN32
  return KnownFolder(FOLDERID_Profile);
#else
  auto size = sysconf(_SC_GETPW_R_SIZE_MAX);
  std::vector<char> buffer(size > 0 ? static_cast<size_t>(size) : 16384);
  for (;;) {
    passwd entry{}, *result = nullptr;
    auto error =
        getpwuid_r(getuid(), &entry, buffer.data(), buffer.size(), &result);
    if (error == ERANGE && buffer.size() < 1024 * 1024) {
      buffer.resize(buffer.size() * 2);
      continue;
    }
    if (error || !result || !entry.pw_dir) return {};
    return fs::path(entry.pw_dir);
  }
#endif
}

// Exclusive creation avoids clobbering any existing file. The fixed probe
// names are retried, so concurrent validation never truncates another probe.
void ProbeWritable(const fs::path& directory) {
  fs::create_directories(directory);
  for (unsigned i = 0; i != 128; ++i) {
    const auto probe =
        directory / (".narutorise-write-check-" + std::to_string(i));
#ifdef _WIN32
    HANDLE file = CreateFileW(
        probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      if (GetLastError() == ERROR_FILE_EXISTS ||
          GetLastError() == ERROR_ALREADY_EXISTS)
        continue;
      throw std::system_error(static_cast<int>(GetLastError()),
                              std::system_category());
    }
    const char byte = 0;
    DWORD written = 0;
    const bool ok = WriteFile(file, &byte, 1, &written, nullptr) &&
                    written == 1 && FlushFileBuffers(file);
    const auto error = GetLastError();
    CloseHandle(file);
    if (!ok)
      throw std::system_error(static_cast<int>(error), std::system_category());
#else
    const int file =
        open(probe.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (file < 0) {
      if (errno == EEXIST) continue;
      throw std::system_error(errno, std::generic_category());
    }
    const char byte = 0;
    const bool ok = write(file, &byte, 1) == 1 && fsync(file) == 0;
    const int error = errno;
    const int closed = close(file);
    const int close_error = errno;
    const int removed = unlink(probe.c_str());
    if (!ok) throw std::system_error(error, std::generic_category());
    if (closed) throw std::system_error(close_error, std::generic_category());
    if (removed) throw std::system_error(errno, std::generic_category());
#endif
    return;
  }
  throw std::runtime_error("Cannot create an exclusive write probe");
}
}  // namespace

Result<Environment> CaptureEnvironment(const fs::path& install_dir,
                                       EnvironmentReader reader,
                                       HomeDiscovery home) {
  try {
    if (!reader) reader = ReadEnvironment;
    if (!home) home = DiscoverHome;
    Environment env;
#ifdef _WIN32
    env.platform = Platform::kWindows;
    env.local_app_data = KnownFolder(FOLDERID_LocalAppData);
#else
    env.platform = Platform::kLinux;
#endif
    env.initial_cwd = fs::current_path();
    env.install_dir = AbsoluteDirectory(install_dir, "Installation directory");
    const auto home_value = reader("HOME");
    if (home_value && !home_value->empty() &&
        ParsePath(*home_value).is_absolute())
      env.home_dir = ParsePath(*home_value);
    else
      env.home_dir = home();
    env.appimage = reader("APPIMAGE").value_or("");
    env.xdg_data_home = reader("XDG_DATA_HOME").value_or("");
    env.xdg_cache_home = reader("XDG_CACHE_HOME").value_or("");
    env.xdg_config_home = reader("XDG_CONFIG_HOME").value_or("");
    env.xdg_state_home = reader("XDG_STATE_HOME").value_or("");
    env.data_root = reader("REX_DATA_ROOT");
    env.user_data_root = reader("REX_USER_DATA_ROOT");
    env.cache_root = reader("REX_CACHE_ROOT");
    return {std::move(env), {}};
  } catch (const std::exception& e) {
    return {std::nullopt, e.what()};
  }
}

Result<DataPaths> ResolvePaths(const Environment& environment,
                               const Overrides& cli) {
  try {
    auto env = environment;
    env.install_dir =
        AbsoluteDirectory(env.install_dir, "Installation directory");
    DataPaths paths;
    paths.install_dir = env.install_dir;
    paths.initial_cwd =
        AbsoluteDirectory(env.initial_cwd, "Initial working directory");
    paths.distribution_dir = paths.install_dir;
    if (env.platform == Platform::kLinux && !env.appimage.empty())
      paths.distribution_dir =
          Resolve(ParsePath(env.appimage), paths.initial_cwd).parent_path();
    paths.portable_descriptor = paths.distribution_dir / "data_location.json";
    paths.session_override =
        cli.data_root.has_value() || (env.data_root && !env.data_root->empty());
    paths.allow_legacy_import = !paths.session_override;
    // A session override must work even if personal discovery/descriptors are
    // inaccessible or malformed. It never consumes the persisted selection.
    if (paths.session_override) {
      paths.data_source =
          cli.data_root ? Source::kCommandLine : Source::kEnvironment;
      paths.data_root =
          Resolve(ParsePath(cli.data_root ? *cli.data_root : *env.data_root),
                  paths.initial_cwd);
      paths.mode = Mode::kCustom;
    } else if (DescriptorExists(paths.portable_descriptor)) {
      ReadDescriptor(paths, env, paths.portable_descriptor);
      paths.data_source = Source::kPortableDescriptor;
    } else {
      if (env.platform == Platform::kWindows)
        paths.user_descriptor =
            AbsoluteDirectory(env.local_app_data, "LocalAppData") /
            "narutorise/data_location.json";
      else
        paths.user_descriptor = XdgBase(env.xdg_config_home, env, ".config") /
                                "narutorise/data_location.json";
      if (DescriptorExists(paths.user_descriptor)) {
        ReadDescriptor(paths, env, paths.user_descriptor);
        paths.data_source = Source::kUserDescriptor;
      } else {
        paths.data_root = DefaultRoot(env);
      }
    }
    paths.config_path = paths.data_root / "narutorise.toml";
    paths.log_dir = paths.data_root / "logs";
    paths.dlc_dir = paths.data_root / "dlc";
    paths.mods_dir = paths.data_root / "mods";
    paths.defaults_path = paths.install_dir / "defaults/narutorise.toml";
    return ApplyStorageOverrides(paths, env, cli, {});
  } catch (const std::exception& e) {
    return {std::nullopt, e.what()};
  }
}

Result<DataPaths> ApplyStorageOverrides(const DataPaths& paths,
                                        const Environment& env,
                                        const Overrides& cli,
                                        const ConfigRoots& config) {
  try {
    auto result = paths;
    const auto config_base =
        config.relative_base.empty()
            ? paths.install_dir
            : AbsoluteDirectory(config.relative_base, "TOML path base");
    auto select = [&](const std::optional<std::string>& command,
                      const std::optional<std::string>& variable,
                      const std::string& configured, fs::path& target,
                      Source& source) {
      source = Source::kDefault;
      if (command) {
        target = Resolve(ParsePath(*command), paths.initial_cwd);
        source = Source::kCommandLine;
      } else if (variable && !variable->empty()) {
        target = Resolve(ParsePath(*variable), paths.initial_cwd);
        source = Source::kEnvironment;
      } else if (!configured.empty()) {
        target = Resolve(ParsePath(configured), config_base);
        source = Source::kConfig;
      }
    };
    result.user_data_root = paths.data_root;
    select(cli.user_data_root, env.user_data_root, config.user_data_root,
           result.user_data_root, result.user_source);
    result.cache_root.clear();
    select(cli.cache_root, env.cache_root, config.cache_root, result.cache_root,
           result.cache_source);
    if (result.cache_source == Source::kDefault)
      result.cache_root = result.user_source == Source::kDefault
                              ? DefaultCache(env, result)
                              : result.user_data_root / "cache";
    return {std::move(result), {}};
  } catch (const std::exception& e) {
    return {std::nullopt, e.what()};
  }
}

Result<DataPaths> PlanLocation(const DataPaths& active, const Environment& env,
                               Mode mode, const fs::path& custom) {
  try {
    if (active.session_override) throw std::runtime_error("Data location is controlled by a session override");
    auto result = active;
    result.mode = mode;
    switch (mode) {
      case Mode::kDefault: result.data_root = DefaultRoot(env); break;
      case Mode::kPortable: result.data_root = active.distribution_dir / "data"; break;
      case Mode::kCustom: result.data_root = Resolve(ParsePath(Utf8(custom)), active.initial_cwd); break;
      default: throw std::runtime_error("Unknown data location mode");
    }
    if (mode == Mode::kPortable || active.data_source == Source::kPortableDescriptor)
      result.selected_descriptor = active.portable_descriptor;
    else {
      result.user_descriptor = env.platform == Platform::kWindows
          ? AbsoluteDirectory(env.local_app_data, "LocalAppData") / "narutorise/data_location.json"
          : XdgBase(env.xdg_config_home, env, ".config") / "narutorise/data_location.json";
      result.selected_descriptor = result.user_descriptor;
    }
    result.data_source = result.selected_descriptor == result.portable_descriptor
        ? Source::kPortableDescriptor : Source::kUserDescriptor;
    result.config_path = result.data_root / "narutorise.toml";
    result.log_dir = result.data_root / "logs";
    result.dlc_dir = result.data_root / "dlc";
    result.mods_dir = result.data_root / "mods";
    result.pending_source = active.data_root;
    return ApplyStorageOverrides(result, env, {}, {});
  } catch (const std::exception& error) { return {std::nullopt, error.what()}; }
}

fs::path CanonicalPath(const fs::path& path) {
  auto ancestor = AbsoluteDirectory(path, "Canonical path");
  fs::path suffix;
  while (!fs::exists(ancestor)) {
    const auto parent = ancestor.parent_path();
    if (parent == ancestor) throw std::runtime_error("No existing path ancestor: " + Utf8(path));
    suffix = suffix.empty() ? ancestor.filename() : ancestor.filename() / suffix;
    ancestor = parent;
  }
  const auto base = fs::canonical(ancestor);
  return suffix.empty() ? base : (base / suffix).lexically_normal();
}

fs::path ResolveDlcSource(const DataPaths& paths, std::string_view configured,
                          const fs::path& relative_base) {
  if (configured.empty()) return {};
  if (configured == "dlc") return paths.dlc_dir;
  return Resolve(ParsePath(std::string(configured)), relative_base);
}

Result<bool> ValidateWritablePaths(const DataPaths& paths) {
  for (const auto& directory :
       std::array{paths.data_root, paths.user_data_root, paths.log_dir,
                  paths.dlc_dir, paths.mods_dir, paths.cache_root}) {
    try {
      AbsoluteDirectory(directory, "Writable directory");
      ProbeWritable(directory);
    } catch (const std::exception& e) {
      return {std::nullopt, "Cannot write to " + Utf8(directory) + ": " +
                                e.what() +
                                ". Choose a writable data folder or use "
                                "--data_root / REX_DATA_ROOT."};
    }
  }
  return {true, {}};
}
}  // namespace narutorise::data
