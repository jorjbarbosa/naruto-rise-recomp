#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace narutorise::data {

template <typename T>
struct Result {
  std::optional<T> value;
  std::string error;
  explicit operator bool() const { return value.has_value(); }
};

enum class Platform { kWindows, kLinux };
enum class Source {
  kDefault,
  kConfig,
  kUserDescriptor,
  kPortableDescriptor,
  kEnvironment,
  kCommandLine
};
enum class Mode { kDefault, kPortable, kCustom };

// The default guest root inside the data folder: the Xbox content trees
// (profiles, shared content such as DLC, title data, achievements) live there,
// apart from the application's files. An explicit user_data_root names the
// guest root itself.
inline constexpr std::string_view kUserFolder = "user";

// All discovered directories are absolute native paths. Tests inject these
// values without changing HOME, Known Folders or any personal storage.
struct Environment {
  Platform platform = Platform::kWindows;
  std::filesystem::path install_dir;
  std::filesystem::path initial_cwd;
  std::filesystem::path home_dir;
  std::filesystem::path local_app_data;
  std::string appimage;
  std::string xdg_data_home, xdg_cache_home, xdg_config_home, xdg_state_home;
  std::optional<std::string> data_root, user_data_root, cache_root;
};

using EnvironmentReader =
    std::function<std::optional<std::string>(std::string_view)>;
using HomeDiscovery = std::function<std::filesystem::path()>;
Result<Environment> CaptureEnvironment(const std::filesystem::path& install_dir,
                                       EnvironmentReader reader = {},
                                       HomeDiscovery home = {});

struct Overrides {
  std::optional<std::string> data_root, user_data_root, cache_root;
};

struct ConfigRoots {
  std::string user_data_root, cache_root;
  // Empty means install_dir, preserving the historical TOML base. Migration
  // may supply a different historical base explicitly.
  std::filesystem::path relative_base;
};

struct DataPaths {
  std::filesystem::path install_dir, distribution_dir, initial_cwd;
  std::filesystem::path data_root, config_path, user_data_root, log_dir,
      dlc_dir, mods_dir, cache_root;
  std::filesystem::path defaults_path;
  // user_descriptor is only discovered when higher-priority selection did
  // not win; unavailable personal storage cannot break an isolated session.
  std::filesystem::path portable_descriptor, user_descriptor,
      selected_descriptor;
  std::optional<std::filesystem::path> pending_source;
  Source data_source = Source::kDefault;
  Source user_source = Source::kDefault;
  Source cache_source = Source::kDefault;
  Mode mode = Mode::kDefault;
  bool session_override = false;
  bool allow_legacy_import = true;
};

// Read-only bootstrap: CLI > environment > distribution descriptor > user
// descriptor > platform default. Descriptor v1 contains version, mode and
// (for custom) path; optional pending_source records a move after restart.
// Relative descriptor paths use the descriptor's parent, never the cwd.
Result<DataPaths> ResolvePaths(const Environment& environment,
                               const Overrides& cli = {});

// Called after locating/parsing the active TOML. Explicit guest roots remain
// exactly that root; no saves/ suffix. CLI/environment relatives use initial
// cwd; TOML relatives use relative_base. The general data root is unchanged.
Result<DataPaths> ApplyStorageOverrides(const DataPaths& paths,
                                        const Environment& environment,
                                        const Overrides& cli,
                                        const ConfigRoots& config);

// The only mutating operation here: create/validate selected writable roots.
// It never changes selection or falls back after an error. Call only after
// migration policy is known, before publishing paths to the runtime.
Result<bool> ValidateWritablePaths(const DataPaths& paths);

// Read-only selection proposal; the active snapshot is never changed. General
// session overrides disable persistent selection. RequestLocation validates and
// publishes the proposal for a subsequent process.
Result<DataPaths> PlanLocation(const DataPaths& active, const Environment& environment,
                              Mode mode, const std::filesystem::path& custom = {});

// Canonicalize the nearest existing ancestor, then append the missing suffix.
// Unlike a root-to-leaf walk, this needs no directory handle for unrelated
// ancestors above the accessible application directory. Errors still propagate.
std::filesystem::path CanonicalPath(const std::filesystem::path& path);

// Empty disables staging; the distributed default follows the active data root.
// Custom relative paths retain their historical executable/TOML base.
std::filesystem::path ResolveDlcSource(const DataPaths& paths,
    std::string_view configured, const std::filesystem::path& relative_base);

}  // namespace narutorise::data
