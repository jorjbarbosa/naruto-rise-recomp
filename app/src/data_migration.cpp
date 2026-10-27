#include "data_migration.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <system_error>
#include <toml++/toml.hpp>
#define XXH_INLINE_ALL
#include <xxhash.h>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace narutorise::data {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;

std::string Utf8(const fs::path& path) {
  const auto text = path.u8string();
  return {text.begin(), text.end()};
}
fs::path Path(const std::string& text) {
  if (text.empty() || text.find('\0') != std::string::npos)
    throw std::runtime_error("Empty path or embedded NUL in migration");
  return fs::path(std::u8string(text.begin(), text.end()));
}
void Require(bool ok, const std::string& error) {
  if (!ok) throw std::runtime_error(error);
}
fs::file_status Status(const fs::path& path) {
  std::error_code ec;
  auto status = fs::symlink_status(path, ec);
  if (ec == std::errc::no_such_file_or_directory) return fs::file_status(fs::file_type::not_found);
  if (ec) throw fs::filesystem_error("Inspect migration path", path, ec);
  return status;
}
bool Exists(const fs::path& path) {
  return Status(path).type() != fs::file_type::not_found;
}
bool Within(const fs::path& child, const fs::path& parent) {
  auto c = child.begin();
  for (auto p = parent.begin(); p != parent.end(); ++p, ++c) {
    if (c == child.end()) return false;
#ifdef _WIN32
    if (CompareStringOrdinal(c->c_str(), -1, p->c_str(), -1, TRUE) != CSTR_EQUAL) return false;
#else
    if (*c != *p) return false;
#endif
  }
  return true;
}
// The storage locations of a preparation (home, known folders, installation,
// and the folders holding the data roots) and their ancestors may be links the
// system or the user made: Wine maps Z:\ to / through one, Fedora Atomic links
// /home to /var/home. Below them, inside the trees read and written, links stay
// refused. Set for the duration of PrepareData/RequestLocation.
thread_local const std::vector<fs::path>* storage_locations = nullptr;
class StorageLocations {
 public:
  StorageLocations() : previous_(storage_locations) { storage_locations = &paths_; }
  ~StorageLocations() { storage_locations = previous_; }
  StorageLocations(const StorageLocations&) = delete;
  StorageLocations& operator=(const StorageLocations&) = delete;
  void Add(const fs::path& path) {
    if (!path.is_absolute()) return;
    paths_.push_back(path.lexically_normal());
    // Paths derived from canonical roots are matched by their canonical form.
    try { paths_.push_back(CanonicalPath(path)); } catch (const std::exception&) {}
  }

 private:
  std::vector<fs::path> paths_;
  const std::vector<fs::path>* previous_;
};
bool StorageLocationOrAncestor(const fs::path& path) {
  return storage_locations && std::any_of(storage_locations->begin(), storage_locations->end(),
      [&](const fs::path& location) { return Within(location, path); });
}
// Reject reparse points as well as C++ symlinks (Windows junctions may be
// reported as directories). Check ancestors before any read/create operation.
void SafePath(const fs::path& path) {
  Require(path.is_absolute(), "Migration path must be absolute: " + Utf8(path));
  fs::path current;
  for (const auto& part : path.lexically_normal()) {
    current /= part;
    if (StorageLocationOrAncestor(current)) continue;
    const auto status = Status(current);
    Require(!fs::is_symlink(status), "Migration refuses link: " + Utf8(current));
#ifdef _WIN32
    auto attributes = GetFileAttributesW(current.c_str());
    Require(attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
            "Migration refuses reparse point: " + Utf8(current));
#endif
  }
}
fs::path Canonical(const fs::path& path) {
  SafePath(path);
  return CanonicalPath(path);
}
void Sync(const fs::path& path, bool directory = false);
void MakeDirectory(const fs::path& path) {
  SafePath(path);
  std::vector<fs::path> missing;
  for (auto current = path; !Exists(current); current = current.parent_path())
    missing.push_back(current);
  fs::create_directories(path);
  for (auto current = missing.rbegin(); current != missing.rend(); ++current) {
    Sync(*current, true);
    Sync(current->parent_path(), true);
  }
}
std::shared_ptr<DataLock> Lock(const fs::path& root) {
  MakeDirectory(root);
  auto file = root / ".narutorise.lock";
  SafePath(file);
#ifdef _WIN32
  auto handle = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  Require(handle != INVALID_HANDLE_VALUE, "Data folder is locked or inaccessible: " + Utf8(root));
  return std::make_shared<DataLock>(reinterpret_cast<std::intptr_t>(handle));
#else
  int handle = open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  Require(handle >= 0, "Cannot open data lock: " + Utf8(root));
  if (flock(handle, LOCK_EX | LOCK_NB)) {
    close(handle);
    throw std::runtime_error("Data folder is locked by another instance: " + Utf8(root));
  }
  return std::make_shared<DataLock>(handle);
#endif
}
void Sync(const fs::path& path, bool directory) {
#ifdef _WIN32
  if (directory) return;  // MoveFileEx uses WRITE_THROUGH below.
  auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  Require(handle != INVALID_HANDLE_VALUE, "Cannot flush migration file: " + Utf8(path));
  bool ok = FlushFileBuffers(handle) != 0;
  CloseHandle(handle);
#else
  int handle = open(path.c_str(), (directory ? O_RDONLY | O_DIRECTORY : O_RDWR) | O_CLOEXEC | O_NOFOLLOW);
  Require(handle >= 0, "Cannot flush migration path: " + Utf8(path));
  bool ok = fsync(handle) == 0;
  close(handle);
#endif
  Require(ok, "Cannot flush migration path: " + Utf8(path));
}
void Replace(const fs::path& from, const fs::path& to) {
  SafePath(from);
  SafePath(to);
#ifdef _WIN32
  Require(MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
          "Cannot commit migration journal: " + Utf8(to));
#else
  fs::rename(from, to);
#endif
  Sync(to.parent_path(), true);
}
void RemoveTemporary(const fs::path& path) {
  SafePath(path);
  if (Exists(path)) {
    Require(fs::is_regular_file(Status(path)), "Invalid migration temporary: " + Utf8(path));
    fs::remove(path);
  }
}
void WriteDescriptor(const fs::path& path, const Json& document) {
  SafePath(path);
  MakeDirectory(path.parent_path());
  auto temporary = path; temporary += ".next";
  RemoveTemporary(temporary);
  {
    std::ofstream output(temporary, std::ios::binary);
    output << document.dump(2) << '\n'; output.flush();
    Require(static_cast<bool>(output), "Cannot write data location: " + Utf8(path));
  }
  Sync(temporary);
  Replace(temporary, path);
}
void WriteState(const fs::path& directory, const Json& state) {
  auto temporary = directory / "state.next";
  RemoveTemporary(temporary);
  {
    std::ofstream output(temporary, std::ios::binary);
    output << state.dump(2) << '\n';
    output.flush();
    Require(static_cast<bool>(output), "Cannot write migration journal: " + Utf8(temporary));
  }
  Sync(temporary);
  Replace(temporary, directory / "state.json");
}
Json ReadState(const fs::path& directory, const fs::path& root) {
  auto path = directory / "state.json";
  SafePath(path);
  if (!Exists(path)) {
    for (const auto& entry : fs::directory_iterator(directory))
      Require(entry.path().filename() == "state.next", "Unrecognized migration metadata without journal: " + Utf8(entry.path()));
    return Json{{"version", 1}, {"destination", Utf8(root)}, {"completed", Json::array()}};
  }
  Require(fs::is_regular_file(Status(path)) && fs::file_size(path) <= 64 * 1024 * 1024,
          "Invalid migration journal: " + Utf8(path));
  std::ifstream input(path, std::ios::binary);
  Require(static_cast<bool>(input), "Cannot read migration journal: " + Utf8(path));
  auto state = Json::parse(input);
  Require(state.at("version") == 1 && state.at("completed").is_array() &&
              (!state.contains("transaction") || state.at("destination") == Utf8(root)),
          "Unsupported migration journal or different destination: " + Utf8(path));
  // A completed portable folder can be moved with its ZIP. An unfinished
  // transaction is tied to the original volume/destination and must be resumed.
  state["destination"] = Utf8(root);
  return state;
}
bool EqualFiles(const fs::path& a, const fs::path& b) {
  SafePath(a);
  SafePath(b);
  if (!fs::is_regular_file(Status(a)) || !fs::is_regular_file(Status(b))) return false;
  if (fs::file_size(a) != fs::file_size(b)) return false;
  std::ifstream left(a, std::ios::binary), right(b, std::ios::binary);
  Require(left && right, "Cannot compare migration files: " + Utf8(a) + " / " + Utf8(b));
  std::array<char, 64 * 1024> x{}, y{};
  do {
    left.read(x.data(), x.size());
    right.read(y.data(), y.size());
    if (left.gcount() != right.gcount() || !std::equal(x.begin(), x.begin() + left.gcount(), y.begin())) return false;
  } while (left.gcount());
  Require(left.eof() && right.eof(), "Read failure comparing migration files: " + Utf8(a));
  return true;
}
std::string FileHash(const fs::path& path) {
  SafePath(path);
  Require(fs::is_regular_file(Status(path)), "Cannot fingerprint migration file: " + Utf8(path));
  std::ifstream input(path, std::ios::binary);
  Require(static_cast<bool>(input), "Cannot read migration file: " + Utf8(path));
  std::unique_ptr<XXH3_state_t, decltype(&XXH3_freeState)> state(XXH3_createState(), XXH3_freeState);
  Require(state && XXH3_128bits_reset(state.get()) != XXH_ERROR, "Cannot create migration fingerprint state");
  std::array<char, 64 * 1024> buffer{};
  while (input.read(buffer.data(), buffer.size()) || input.gcount())
    Require(XXH3_128bits_update(state.get(), buffer.data(), static_cast<size_t>(input.gcount())) != XXH_ERROR,
            "Cannot fingerprint migration file: " + Utf8(path));
  Require(input.eof(), "Read failure fingerprinting migration file: " + Utf8(path));
  XXH128_canonical_t digest;
  XXH128_canonicalFromHash(&digest, XXH3_128bits_digest(state.get()));
  constexpr char hex[] = "0123456789abcdef";
  std::string text;
  for (auto byte : digest.digest) {
    text += hex[byte >> 4];
    text += hex[byte & 15];
  }
  return text;
}
void CopyVerified(const fs::path& source, const fs::path& destination) {
  SafePath(source);
  SafePath(destination);
  Require(!Exists(destination), "Migration temporary already exists: " + Utf8(destination));
  {
    std::ifstream input(source, std::ios::binary);
    std::ofstream output(destination, std::ios::binary);
    Require(input && output, "Cannot stage migration file: " + Utf8(source));
    std::array<char, 64 * 1024> buffer{};
    while (input.read(buffer.data(), buffer.size()) || input.gcount()) {
      output.write(buffer.data(), input.gcount());
      Require(static_cast<bool>(output), "Write failed staging file: " + Utf8(source));
    }
    output.flush();
    Require(input.eof() && output, "I/O failure staging file: " + Utf8(source));
  }
  Sync(destination);
  Require(EqualFiles(source, destination), "Source changed during copy: " + Utf8(source));
}
void Publish(const fs::path& staged, const fs::path& destination, const fs::path& temporary) {
  RemoveTemporary(temporary);
  CopyVerified(staged, temporary);
  SafePath(destination);
#ifdef _WIN32
  Require(MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) != 0,
          "Cannot publish migration file (destination preserved): " + Utf8(destination));
#else
  // link is an atomic, no-replace publication on the same destination volume.
  // Unlike rename(), it cannot overwrite a file that appeared after preflight.
  Require(link(temporary.c_str(), destination.c_str()) == 0,
          "Cannot publish migration file (destination preserved): " + Utf8(destination));
  fs::remove(temporary);
#endif
  Sync(destination.parent_path(), true);
}
fs::path DiscoverDocuments() {
#ifdef _WIN32
  PWSTR value = nullptr;
  auto result = SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DONT_VERIFY, nullptr, &value);
  Require(SUCCEEDED(result), "Cannot discover Windows Documents; select --data_root to use an isolated folder");
  fs::path path(value);
  CoTaskMemFree(value);
  return path;
#else
  throw std::runtime_error("Windows Documents discovery requires Windows or an injected fixture");
#endif
}
fs::path Xdg(const std::string& value, const Environment& env, const fs::path& fallback) {
  if (!value.empty() && Path(value).is_absolute()) return Path(value).lexically_normal();
  Require(env.home_dir.is_absolute(), "Cannot discover legacy XDG directory");
  return env.home_dir / fallback;
}
void AddStorageLocations(StorageLocations& locations, const DataPaths& paths, const Environment& env) {
  for (const auto& location : {paths.install_dir, paths.distribution_dir, env.home_dir,
                               env.local_app_data, paths.data_root.parent_path()})
    locations.Add(location);
  if (paths.pending_source) locations.Add(paths.pending_source->parent_path());
  if (env.platform == Platform::kLinux && env.home_dir.is_absolute())
    for (const auto& [value, fallback] : {std::pair{&env.xdg_data_home, ".local/share"},
                                          std::pair{&env.xdg_config_home, ".config"},
                                          std::pair{&env.xdg_cache_home, ".cache"},
                                          std::pair{&env.xdg_state_home, ".local/state"}})
      locations.Add(Xdg(*value, env, fallback));
}
bool HexName(const fs::path& path, size_t size) {
  auto name = Utf8(path.filename());
  return name.size() == size && std::all_of(name.begin(), name.end(), [](unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  });
}
Json MakeSource(std::string_view kind, const fs::path& from, const fs::path& to) {
  return Json{{"kind", kind}, {"from", Utf8(from.lexically_normal())}, {"to", to.empty() ? "" : Utf8(to)}};
}
std::string Identity(const Json& source) { return source.dump(); }
bool Completed(const Json& state, const Json& source) {
  const auto id = Identity(source);
  const auto& completed = state.at("completed");
  return std::find(completed.begin(), completed.end(), id) != completed.end();
}
fs::path Destination(const fs::path& root, const std::string& text) {
  if (text.empty()) return root;
  auto relative = Path(text);
  Require(!relative.has_root_path(), "Absolute destination in migration inventory");
  for (const auto& part : relative) Require(part != ".." && part != ".", "Invalid migration inventory destination");
  return root / relative;
}
void AddTree(const fs::path& from, const fs::path& to, Json& inventory, Json& skipped,
             bool skip_stfs_stage = true) {
  SafePath(from);
  auto status = Status(from);
  if (fs::is_directory(status)) {
    inventory.push_back(Json{{"from", Utf8(from)}, {"to", Utf8(to)}, {"directory", true}});
    std::vector<fs::path> children;
    for (const auto& child : fs::directory_iterator(from)) children.push_back(child.path());
    std::sort(children.begin(), children.end());
    for (const auto& child : children) {
      if (skip_stfs_stage && child.filename() == ".stfs_stage") skipped.push_back(Utf8(child));
      else AddTree(child, to / child.filename(), inventory, skipped, skip_stfs_stage);
    }
  } else {
    Require(fs::is_regular_file(status), "Migration refuses unsupported file: " + Utf8(from));
    inventory.push_back(Json{{"from", Utf8(from)}, {"to", Utf8(to)}, {"directory", false},
        {"size", fs::file_size(from)}, {"hash", FileHash(from)}});
  }
}
Json Inventory(const Json& sources, const fs::path& root, Json& skipped) {
  Json inventory = Json::array();
  for (const auto& source : sources) {
    auto from = Canonical(Path(source.at("from").get<std::string>()));
    auto to = Canonical(Destination(root, source.at("to").get<std::string>()));
    if (Within(from, to) && Within(to, from)) continue;
    Require(!Within(from, to) && !Within(to, from),
            "Migration source and destination overlap: " + Utf8(from) + " / " + Utf8(to));
    if (!Exists(from)) continue;
    const auto kind = source.at("kind").get<std::string>();
    if (kind == "guest") {
      Require(fs::is_directory(Status(from)), "Legacy guest root is not a directory: " + Utf8(from));
      std::vector<fs::path> children;
      for (const auto& child : fs::directory_iterator(from)) children.push_back(child.path());
      std::sort(children.begin(), children.end());
      for (const auto& child : children) {
        if (HexName(child, 16) || HexName(child, 8) || child.filename() == "achievements") {
          Require(fs::is_directory(Status(child)), "Expected guest directory: " + Utf8(child));
          AddTree(child, child.filename(), inventory, skipped);
        } else skipped.push_back(Utf8(child));
      }
    } else {
      Require(kind == "config" || kind == "logs" || kind == "dlc" || kind == "mods" ||
                  kind == "bundled_mod", "Unknown migration source kind");
      AddTree(from, Path(source.at("to").get<std::string>()), inventory, skipped,
              kind != "mods" && kind != "bundled_mod");
    }
  }
  return inventory;
}
toml::table ReadConfig(const fs::path& path) {
  SafePath(path);
  Require(fs::is_regular_file(Status(path)), "Configuration is not a regular file: " + Utf8(path));
  std::ifstream input(path, std::ios::binary);
  Require(static_cast<bool>(input), "Cannot read configuration: " + Utf8(path));
  try {
    return toml::parse(input);
  } catch (const toml::parse_error& e) {
    throw std::runtime_error("Invalid TOML (file preserved): " + Utf8(path) + ": " + std::string(e.description()));
  }
}
ConfigRoots Roots(const toml::table& table, const fs::path& base) {
  ConfigRoots roots;
  roots.relative_base = base;
  for (auto [key, output] : {std::pair{"user_data_root", &roots.user_data_root},
                             std::pair{"cache_root", &roots.cache_root}}) {
    if (auto node = table[key]) {
      auto value = node.value<std::string>();
      Require(value.has_value(), std::string("Expected string in configuration: ") + key);
      *output = *value;
    }
  }
  return roots;
}
fs::path AppRunConfigBase(const fs::path& config, const toml::table& table,
                           const Environment& env) {
  bool relative = false;
  for (const char* key : {"user_data_root", "cache_root", "dlc_source_path"}) {
    const auto text = table[key].value_or(std::string());
    if (!text.empty() && !(std::string_view(key) == "dlc_source_path" && text == "dlc") &&
        Path(text).is_relative()) relative = true;
  }
  if (!relative) return env.install_dir;
  // Old AppRun symlinked its cached executable's TOML to this exact XDG file.
  // Inspect only that known wrapper link, never follow arbitrary guest links.
  auto matches = [&](const fs::path& directory) {
    SafePath(directory);
    auto link = directory / "narutorise.toml";
    if (!fs::is_symlink(Status(link))) return false;
    auto target = fs::read_symlink(link);
    if (target.is_relative()) target = directory / target;
    return target.lexically_normal() == config.lexically_normal();
  };
  if (matches(env.install_dir)) return env.install_dir;
  const auto cache = Xdg(env.xdg_cache_home, env, ".cache") / "narutorise/appimage";
  SafePath(cache);
  std::vector<fs::path> candidates;
  if (Exists(cache)) for (const auto& entry : fs::directory_iterator(cache)) {
    SafePath(entry.path());
    const auto ready = entry.path() / ".ready";
    if (fs::is_directory(Status(entry.path())) && Exists(ready)) {
      SafePath(ready);
      if (matches(entry.path())) candidates.push_back(entry.path());
    }
  }
  Require(candidates.size() == 1,
          "Cannot determine the historical AppRun executable directory for relative paths in " + Utf8(config) +
          ". Restore the original payload cache or change those paths to absolute locations before retrying. "
          "Use --data_root for an isolated session.");
  return candidates.front();
}
void Checkpoint(const MigrationOptions& options, std::string_view event, const fs::path& path) {
  if (options.checkpoint) options.checkpoint(event, path);
}
// APPDIR changes between invocations. Only explicitly marked immutable package
// resources may be rebound; the full inventory must still match byte hashes.
// Personal/legacy sources retain their original paths and recovery semantics.
void RebaseBundledSources(Json& state, const DataPaths& paths, const fs::path& metadata) {
  auto candidate = state;
  auto& transaction = candidate.at("transaction");
  bool changed = false;
  for (auto& source : transaction.at("sources")) {
    if (!source.contains("resource")) continue;
    const auto resource = source.at("resource").get<std::string>();
    Require((source.at("kind") == "config" && resource == "defaults/narutorise.toml") ||
                (source.at("kind") == "bundled_mod" && resource == "defaults/mods/pt-br"),
            "Invalid bundled migration resource");
    const auto previous = Path(source.at("from").get<std::string>());
    const auto current = Canonical(paths.install_dir / Path(resource));
    if (previous == current) continue;
    for (auto& entry : transaction.at("inventory")) {
      const auto from = Path(entry.at("from").get<std::string>());
      if (Within(from, previous)) {
        const auto suffix = from.lexically_relative(previous);
        entry["from"] = Utf8(suffix == "." ? current : current / suffix);
      }
    }
    source["from"] = Utf8(current);
    changed = true;
  }
  if (!changed) return;
  Json skipped = Json::array();
  Require(Inventory(transaction.at("sources"), paths.data_root, skipped) == transaction.at("inventory"),
          "Bundled resources changed during interrupted preparation. Resume with the original package; stage and active data preserved.");
  WriteState(metadata, candidate);
  state = std::move(candidate);
}
void VerifySnapshot(const Json& transaction, const fs::path& root, const fs::path& metadata, bool destination) {
  Json skipped = Json::array();
  Require(Inventory(transaction.at("sources"), root, skipped) == transaction.at("inventory"),
          "Legacy source inventory changed. Originals and migration stage preserved; restore the source before retrying.");
  size_t index = 0;
  for (const auto& entry : transaction.at("inventory")) {
    auto staged = metadata / (std::to_string(index++) + ".bin");
    if (entry.at("directory").get<bool>()) {
      if (destination) {
        auto to = Destination(root, entry.at("to").get<std::string>());
        SafePath(to);
        Require(fs::is_directory(Status(to)), "Published directory changed: " + Utf8(to));
      }
      continue;
    }
    auto from = Path(entry.at("from").get<std::string>());
    Require(EqualFiles(from, staged), "Source changed since staging: " + Utf8(from) + ". Original and stage preserved.");
    if (destination) {
      auto to = Destination(root, entry.at("to").get<std::string>());
      Require(EqualFiles(staged, to), "Published data changed: " + Utf8(to));
    }
  }
}
void Migrate(Json& state, const fs::path& root, const fs::path& metadata, const MigrationOptions& options) {
  const auto& transaction = state.at("transaction");
  const auto& inventory = transaction.at("inventory");
  Json skipped = Json::array();
  Require(Inventory(transaction.at("sources"), root, skipped) == inventory,
          "Legacy source inventory changed; original files and migration journal preserved");
  Checkpoint(options, "inventoried", root);
  size_t index = 0;
  for (const auto& entry : inventory) {
    auto staged = metadata / (std::to_string(index++) + ".bin");
    if (entry.at("directory").get<bool>()) continue;
    auto from = Path(entry.at("from").get<std::string>());
    Require(FileHash(from) == entry.at("hash").get<std::string>(), "Source changed since inventory: " + Utf8(from));
    SafePath(staged);
    if (!Exists(staged)) {
      auto temporary = metadata / "copy.part";
      RemoveTemporary(temporary);
      CopyVerified(from, temporary);
      Replace(temporary, staged);
    }
    Require(EqualFiles(from, staged), "Source changed since staging: " + Utf8(from) + ". Original and stage preserved.");
    Checkpoint(options, "staged", from);
  }
  VerifySnapshot(transaction, root, metadata, false);
  // Preflight every conflict before publishing anything, including directories.
  for (const auto& entry : inventory) {
    auto to = Destination(root, entry.at("to").get<std::string>());
    SafePath(to);
    if (!Exists(to)) continue;
    bool same = entry.at("directory").get<bool>() ? fs::is_directory(Status(to))
        : EqualFiles(Path(entry.at("from").get<std::string>()), to);
    Require(same, "Migration conflict: " + entry.at("from").get<std::string>() + " / " + Utf8(to) +
                  ". Both copies preserved. Back up and resolve the differing files, then retry.");
  }
  index = 0;
  for (const auto& entry : inventory) {
    auto staged = metadata / (std::to_string(index++) + ".bin");
    auto to = Destination(root, entry.at("to").get<std::string>());
    Checkpoint(options, "publishing", to);
    if (entry.at("directory").get<bool>()) MakeDirectory(to);
    else if (!Exists(to)) {
      MakeDirectory(to.parent_path());
      Publish(staged, to, metadata / "publish.part");
    } else Require(EqualFiles(staged, to), "Destination changed during publication: " + Utf8(to));
    Checkpoint(options, "published", to);
  }
  VerifySnapshot(transaction, root, metadata, true);
  Checkpoint(options, "verified", root);
  // Keep the transaction until the completion record is atomically committed.
  for (const auto& source : transaction.at("sources")) state["completed"].push_back(Identity(source));
  // Carry source completion records with a user-selected move. Once the
  // pending descriptor is cleared, stale Documents/AppRun data must not be
  // rediscovered and imported over newer saves in this location.
  for (const auto& id : transaction.value("inherited_completed", Json::array()))
    if (std::find(state["completed"].begin(), state["completed"].end(), id) == state["completed"].end())
      state["completed"].push_back(id);
  state["config_base"] = transaction.at("config_base");
  state["skipped"] = transaction.at("skipped");
  state.erase("transaction");
  WriteState(metadata, state);
  Checkpoint(options, "committed", root);
}
void CleanStage(const fs::path& metadata) {
  // Only our numbered stage files and exact temporary names; no recursive
  // deletion and no deletion of unrecognized files in user storage.
  for (const auto& entry : fs::directory_iterator(metadata)) {
    const auto name = Utf8(entry.path().filename());
    const auto stem = Utf8(entry.path().stem());
    const bool numbered = entry.path().extension() == ".bin" && !stem.empty() &&
        std::all_of(stem.begin(), stem.end(), [](char c) { return c >= '0' && c <= '9'; });
    if (numbered || name == "copy.part" || name == "publish.part" || name == "state.next")
      RemoveTemporary(entry.path());
  }
}
}  // namespace

DataLock::~DataLock() {
#ifdef _WIN32
  CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
  close(static_cast<int>(handle_));
#endif
}

Result<PreparedData> PrepareData(const DataPaths& paths, const Environment& env,
                                 const Overrides& cli, const MigrationOptions& options) {
  try {
    StorageLocations locations;
    AddStorageLocations(locations, paths, env);
    if (!options.legacy_install_dir.empty()) locations.Add(options.legacy_install_dir);
    PreparedData prepared;
    const auto root = Canonical(paths.data_root);
    prepared.lease = Lock(root);
    const auto metadata = root / ".narutorise-migration";
    MakeDirectory(metadata);
    auto state = ReadState(metadata, root);
    std::shared_ptr<DataLock> previous_lease;
    Json inherited_completed = Json::array();
    bool previous_complete = false;
    if (paths.allow_legacy_import && paths.pending_source) {
      auto previous = Canonical(*paths.pending_source);
      Require(previous != root && !Within(previous, root) && !Within(root, previous),
              "Pending source and destination overlap");
      previous_complete = !state.contains("transaction") &&
          Completed(state, MakeSource("logs", *paths.pending_source / "logs", "logs")) &&
          Completed(state, MakeSource("dlc", *paths.pending_source / "dlc", "dlc")) &&
          Completed(state, MakeSource("mods", *paths.pending_source / "mods", "mods"));
      if (!previous_complete) {
        Require(fs::is_directory(Status(previous)), "Pending source is missing: " + Utf8(previous));
        previous_lease = Lock(previous);
        const auto previous_metadata = previous / ".narutorise-migration";
        SafePath(previous_metadata);
        if (Exists(previous_metadata / "state.json")) {
          const auto prior = ReadState(previous_metadata, previous);
          Require(!prior.contains("transaction"), "Pending source has an incomplete migration");
          inherited_completed = prior.at("completed");
        }
      }
    }
    const auto request = Json{{"guest_cli", cli.user_data_root.value_or("")},
        {"guest_env", env.user_data_root.value_or("")},
        {"pending", paths.pending_source ? Utf8(*paths.pending_source) : ""}};
    if (state.contains("transaction")) {
      Require(paths.allow_legacy_import || state["transaction"].value("isolated", false),
              "Incomplete legacy migration in this folder. Resume without a session data override first.");
      Require(state["transaction"].at("request") == request,
              "Incomplete migration has different guest overrides or pending source. Resume with the original selection first.");
      RebaseBundledSources(state, paths, metadata);
      Migrate(state, root, metadata, options);
    }
    CleanStage(metadata);
    auto base = state.contains("config_base") ? Path(state.at("config_base").get<std::string>()) : paths.install_dir;
    auto config = paths.config_path;
    bool legacy_xdg = false;
    Json sources = Json::array();
    auto add = [&](const Json& source) {
      if (!Completed(state, source)) sources.push_back(source);
    };
    if (!Exists(config)) {
      base = options.legacy_install_dir.empty() ? paths.install_dir : options.legacy_install_dir;
      if (paths.allow_legacy_import) {
        if (paths.pending_source) {
          config = *paths.pending_source / "narutorise.toml";
          auto previous_metadata = *paths.pending_source / ".narutorise-migration";
          SafePath(previous_metadata);
          if (Exists(previous_metadata / "state.json")) {
            auto previous = ReadState(previous_metadata, Canonical(*paths.pending_source));
            Require(!previous.contains("transaction"), "Pending source has an incomplete migration");
            if (previous.contains("config_base")) base = Path(previous.at("config_base").get<std::string>());
          }
        } else {
          config = paths.install_dir / "narutorise.toml";
          if (env.platform == Platform::kLinux) {
            auto xdg = Xdg(env.xdg_config_home, env, ".config") / "narutorise/narutorise.toml";
            if (Exists(xdg)) { config = xdg; legacy_xdg = true; }
          }
        }
      }
      if (!paths.allow_legacy_import || !Exists(config)) {
        config = paths.defaults_path;
        base = paths.install_dir;
      }
      auto source = MakeSource("config", config, "narutorise.toml");
      if (config == paths.defaults_path) source["resource"] = "defaults/narutorise.toml";
      add(source);
    }
    const auto config_hash = FileHash(config);
    auto table = ReadConfig(config);
    Require(FileHash(config) == config_hash, "Configuration changed while reading storage settings; retry preparation");
    if (legacy_xdg && options.legacy_install_dir.empty() && !env.appimage.empty())
      base = AppRunConfigBase(config, table, env);
    prepared.config_roots = Roots(table, base);
    auto effective = ApplyStorageOverrides(paths, env, cli, prepared.config_roots);
    Require(static_cast<bool>(effective), effective.error);
    prepared.paths = std::move(*effective.value);
    prepared.config_base = base;
    if (paths.allow_legacy_import && !previous_complete) {
      if (prepared.paths.user_source == Source::kDefault) {
        fs::path guest;
        if (paths.pending_source) guest = *paths.pending_source;
        else if (env.platform == Platform::kWindows) {
          auto documents = options.documents ? options.documents() : DiscoverDocuments();
          locations.Add(documents);
          SafePath(documents);
          Require(fs::is_directory(Status(documents)),
                  "Windows Documents is unavailable: " + Utf8(documents) +
                  ". Reconnect the original storage or choose --data_root for an isolated session.");
          guest = documents / "narutorise";
        }
        else guest = Xdg(env.xdg_data_home, env, ".local/share") / "narutorise";
        add(MakeSource("guest", guest, {}));
      }
      auto log_source = paths.install_dir / "logs";
      if (env.platform == Platform::kLinux) {
        auto xdg = Xdg(env.xdg_state_home, env, ".local/state") / "narutorise/logs";
        if (Exists(xdg)) log_source = xdg;
      }
      if (paths.pending_source) {
        add(MakeSource("logs", *paths.pending_source / "logs", "logs"));
        add(MakeSource("dlc", *paths.pending_source / "dlc", "dlc"));
        add(MakeSource("mods", *paths.pending_source / "mods", "mods"));
      } else {
        add(MakeSource("logs", log_source, env.platform == Platform::kLinux ? "logs/legacy/xdg" : "logs/legacy/install"));
        add(MakeSource("dlc", paths.install_dir / "dlc", "dlc"));
        add(MakeSource("mods", paths.install_dir / "mods", "mods"));
        if (paths.distribution_dir != paths.install_dir)
          add(MakeSource("mods", paths.distribution_dir / "mods", "mods"));
        // Older AppRun versions staged packages in each writable payload cache.
        // Merge every completed deployment conservatively; conflicting bytes
        // block migration instead of selecting a build by modification time.
        if (env.platform == Platform::kLinux && !env.appimage.empty()) {
          const auto cache = Xdg(env.xdg_cache_home, env, ".cache") / "narutorise/appimage";
          SafePath(cache);
          if (Exists(cache)) for (const auto& entry : fs::directory_iterator(cache)) {
            SafePath(entry.path());
            if (!fs::is_directory(Status(entry.path()))) continue;
            const auto ready = entry.path() / ".ready";
            SafePath(ready);
            if (Exists(ready)) {
              add(MakeSource("dlc", entry.path() / "dlc", "dlc"));
              add(MakeSource("mods", entry.path() / "mods", "mods"));
            }
          }
        }
      }
    }
    Checkpoint(options, "configured", config);
    auto migrate = [&] {
      if (sources.empty()) return;
      Json skipped = Json::array();
      auto inventory = Inventory(sources, root, skipped);
      Require(FileHash(config) == config_hash,
              "Configuration changed before migration inventory; original and destination preserved, retry preparation");
      state["transaction"] = Json{{"sources", sources}, {"inventory", inventory},
          {"config_base", Utf8(base)}, {"skipped", skipped}, {"isolated", !paths.allow_legacy_import},
          {"inherited_completed", inherited_completed},
          {"request", request}};
      WriteState(metadata, state);
      Migrate(state, root, metadata, options);
      CleanStage(metadata);
    };
    migrate();
    // Legacy/custom packages win as a whole, including partial packages. Only
    // the curated, immutable seed is eligible in isolated CLI/env sessions.
    // This is a separate recoverable transaction, after legacy mods have been
    // published, so a user's PT-BR never conflicts with the factory copy.
    const auto bundled = paths.install_dir / "defaults/mods/pt-br";
    SafePath(prepared.paths.mods_dir / "pt-br");
    SafePath(bundled);
    if (!Exists(prepared.paths.mods_dir / "pt-br") && Exists(bundled)) {
      Require(fs::is_directory(Status(bundled)), "Bundled PT-BR is not a directory: " + Utf8(bundled));
      auto source = MakeSource("bundled_mod", bundled, "mods/pt-br");
      source["resource"] = "defaults/mods/pt-br";
      sources = Json::array({source});
      migrate();
    }
    // Parsing again also checks the published file, including on resume.
    prepared.config_roots = Roots(ReadConfig(paths.config_path), base);
    Require(FileHash(paths.config_path) == config_hash,
            "Configuration changed during data preparation; no runtime paths published, retry preparation");
    if (paths.pending_source && !paths.selected_descriptor.empty()) {
      // Confirm the selection still names this transaction before removing the
      // pending marker. If this write is interrupted, the completed journal
      // permits retry without requiring the original folder again.
      auto current = ResolvePaths(env, cli);
      Require(current && current.value->selected_descriptor == paths.selected_descriptor &&
              current.value->data_root == paths.data_root && current.value->pending_source == paths.pending_source,
              "Data location selection changed during preparation");
      SafePath(paths.selected_descriptor);
      Json document;
      { std::ifstream input(paths.selected_descriptor, std::ios::binary); document = Json::parse(input); }
      document.erase("pending_source");
      WriteDescriptor(paths.selected_descriptor, document);
      prepared.paths.pending_source.reset();
    }
    for (const auto& item : state.value("skipped", Json::array()))
      prepared.diagnostics.push_back("Not imported: " + item.get<std::string>());
    return {std::move(prepared), {}};
  } catch (const std::exception& e) {
    return {std::nullopt, "Data preparation failed for " + Utf8(paths.data_root) + ": " + e.what()};
  }
}
Result<DataPaths> RequestLocation(const PreparedData& active, const Environment& env,
    Mode mode, const fs::path& custom, const Overrides& cli) {
  try {
    Require(static_cast<bool>(active.lease), "Active data lease is unavailable");
    auto planned = PlanLocation(active.paths, env, mode, custom);
    Require(static_cast<bool>(planned), planned.error);
    auto effective = ApplyStorageOverrides(*planned.value, env, cli, active.config_roots);
    Require(static_cast<bool>(effective), effective.error);
    auto result = std::move(*effective.value);
    StorageLocations locations;
    AddStorageLocations(locations, active.paths, env);
    AddStorageLocations(locations, result, env);
    const auto previous = Canonical(active.paths.data_root), next = Canonical(result.data_root);
    const bool same = Within(previous, next) && Within(next, previous);
    Require(same || (!Within(previous, next) && !Within(next, previous)),
            "Choose a data folder separate from the active folder (not a parent or child)");
    std::shared_ptr<DataLock> target_lease;
    if (!same) target_lease = Lock(next);
    const auto writable = ValidateWritablePaths(result);
    Require(static_cast<bool>(writable), writable.error);
    Json document{{"version", 1}, {"mode", mode == Mode::kPortable ? "portable" : mode == Mode::kCustom ? "custom" : "default"}};
    if (mode == Mode::kCustom) document["path"] = Utf8(result.data_root);
    if (!same) document["pending_source"] = Utf8(active.paths.data_root);
    else result.pending_source.reset();
    WriteDescriptor(result.selected_descriptor, document);
    return {std::move(result), {}};
  } catch (const std::exception& error) { return {std::nullopt, error.what()}; }
}
}  // namespace narutorise::data
