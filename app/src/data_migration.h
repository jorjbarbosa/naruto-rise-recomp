#pragma once

#include "data_paths.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace narutorise::data {

// Hold this lease for the entire lifetime of all writers using this data root.
// The OS releases it on process death; the lock file is deliberately retained.
class DataLock {
 public:
  explicit DataLock(std::intptr_t handle) : handle_(handle) {}
  ~DataLock();
  DataLock(const DataLock&) = delete;
  DataLock& operator=(const DataLock&) = delete;

 private:
  std::intptr_t handle_;
};

struct MigrationOptions {
  // Production default: SHGetKnownFolderPath(FOLDERID_Documents). Tests inject
  // a fixture, never redirect the user's actual Known Folder.
  std::function<std::filesystem::path()> documents;
  // Historical executable directory, if different from install_dir (for
  // example an earlier AppRun deployment). Persisted with imported config.
  std::filesystem::path legacy_install_dir;
  // Optional fault/observation hook. Throwing stops preparation without a
  // usable result. Events: configured, inventoried, staged, publishing, published, verified, committed.
  std::function<void(std::string_view, const std::filesystem::path&)> checkpoint;
};

struct PreparedData {
  DataPaths paths;
  ConfigRoots config_roots;
  // Consumers of other historical relative TOML paths must use this base too.
  std::filesystem::path config_base;
  std::vector<std::string> diagnostics;
  std::shared_ptr<DataLock> lease;
};

// Acquires the root lease, validates active/imported TOML, discovers eligible
// sources and copies them without deleting originals or overwriting divergent
// destinations. Success is the only permission to publish paths/start guest.
// No runtime/SDK dependency. Session data overrides skip personal discovery.
Result<PreparedData> PrepareData(const DataPaths& paths,
                                 const Environment& environment,
                                 const Overrides& cli = {},
                                 const MigrationOptions& options = {});

// Validate destination/overlap/writers and atomically persist a pending
// selection. Files remain in the active root until PrepareData on next boot.
Result<DataPaths> RequestLocation(const PreparedData& active, const Environment& environment,
    Mode mode, const std::filesystem::path& custom = {}, const Overrides& cli = {});

}  // namespace narutorise::data
