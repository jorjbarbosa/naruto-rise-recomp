#include "data_migration.h"

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
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace narutorise::data;
namespace {
int checks = 0;
fs::path executable;
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
  auto text = path.u8string();
  return {text.begin(), text.end()};
}
fs::path Path(std::string_view text) { return fs::path(std::u8string(text.begin(), text.end())); }
void Write(const fs::path& path, const std::string& data) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output.write(data.data(), data.size());
  if (!output) throw std::runtime_error("Cannot write fixture: " + Utf8(path));
}
std::string Read(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot read fixture: " + Utf8(path));
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
using Files = std::map<fs::path, std::string>;
Files Snapshot(const fs::path& root) {
  Files result;
  if (fs::exists(root)) for (const auto& entry : fs::recursive_directory_iterator(root))
    if (entry.is_regular_file()) result[entry.path().lexically_relative(root)] = Read(entry.path());
  return result;
}
struct Fixture {
  Environment env;
  MigrationOptions options;
  fs::path documents, root;
  explicit Fixture(const fs::path& directory, Platform platform = Platform::kWindows) : root(directory) {
    env.platform = platform;
    env.install_dir = root / "installation";
    env.initial_cwd = root / "other cwd";
    env.local_app_data = root / "LocalAppData";
    env.home_dir = root / "home";
    documents = root / Path("OneDrive redirecionado/Documentos 日本語");
    auto docs = documents;
    options.documents = [docs] { return docs; };
    fs::create_directories(documents);
    Write(env.install_dir / "defaults/narutorise.toml", "renderer = 'native'\n");
    fs::create_directories(env.initial_cwd);
  }
  DataPaths Paths(const Overrides& cli = {}) const { return Value(ResolvePaths(env, cli)); }
  fs::path Guest() const {
    return env.platform == Platform::kWindows ? documents / "narutorise" : env.home_dir / ".local/share/narutorise";
  }
  Result<PreparedData> Prepare(const Overrides& cli = {}) const {
    return PrepareData(Paths(cli), env, cli, options);
  }
};
void GuestTree(const fs::path& root) {
  Write(root / "E000000000000001/555307E5/00000001/PlayerSave1.ninjasave/SaveData", std::string("save\0bytes", 10));
  Write(root / "E000000000000001/555307E5/Headers/00000001/PlayerSave1.ninjasave.header", "license+header");
  Write(root / "E000000000000002/555307E5/00000001/Save2/data", "second profile");
  Write(root / "0000000000000000/555307E5/00000002/DLC/payload", "installed dlc");
  Write(root / "0000000000000000/555307E5/Headers/00000002/DLC.header", "dlc license");
  Write(root / Path("555307E5/profile/Jogador 日本語/settings"), "profile state");
  Write(root / "achievements/555307E5.json", "unlocked");
  fs::create_directories(root / "E000000000000003/555307E5/empty");
}
void Mods(const fs::path& directory, Platform platform) {
  Fixture f(directory / "legacy", platform);
  const auto legacy = f.env.install_dir / "mods";
  const auto seed = f.env.install_dir / "defaults/mods/pt-br";
  Write(legacy / "pt-br/texts.json", "personal translation");
  Write(legacy / Path("community/arquivo 日本語.bin"), std::string("mod\0bytes", 9));
  Write(legacy / "community/.stfs_stage/payload", "mod-owned resource, not DLC staging");
  Write(seed / "texts.json", "factory v1");
  Write(seed / "mod.toml", "factory manifest");
  Write(f.env.install_dir / "narutorise.toml", "translation_mod = 'pt-br'\nuser_language = 1\n");
  const auto originals = Snapshot(legacy);
  f.options.checkpoint = [](std::string_view event, const fs::path& path) {
    if (event == "published" && path.filename() == "texts.json")
      throw std::runtime_error("interrupt mods publication");
  };
  Check(!f.Prepare(), "interrupted mods cannot publish runtime snapshot");
  f.options.checkpoint = {};
  auto active = Value(f.Prepare());
  Check(Snapshot(active.paths.mods_dir) == originals, "all legacy mods resume byte-for-byte; seed preserves partial custom package");
  Check(Snapshot(legacy) == originals, "legacy originals untouched");
  Check(Read(active.paths.config_path).find("translation_mod = 'pt-br'") != std::string::npos,
        "translation selection preserved in active TOML");
  Write(seed / "texts.json", "factory v2");
  Write(active.paths.mods_dir / "pt-br/texts.json", "active personalization");
  active.lease.reset();
  active = Value(f.Prepare());
  Check(Read(active.paths.mods_dir / "pt-br/texts.json") == "active personalization" &&
            !fs::exists(active.paths.mods_dir / "pt-br/mod.toml"),
        "rebuild/update preserves complete active package policy");
  const auto before_move = Snapshot(active.paths.mods_dir);
  const auto destination = f.root / Path("dados novos 日本語 com espaços");
  Value(RequestLocation(active, f.env, Mode::kCustom, destination));
  Check(Snapshot(active.paths.mods_dir) == before_move && fs::is_empty(destination / "mods"),
        "pending choice leaves active mods in source");
  active.lease.reset();
  auto moved = Value(f.Prepare());
  Check(moved.paths.mods_dir == destination / "mods" && Snapshot(moved.paths.mods_dir) == before_move,
        "mods follow activation after restart");
  Check(Read(moved.paths.config_path).find("translation_mod = 'pt-br'") != std::string::npos,
        "translation selection follows restart");
  moved.lease.reset();
  moved = Value(f.Prepare());
  Check(Snapshot(moved.paths.mods_dir) == before_move, "inherited completion prevents stale mods reimport");
  Check(Snapshot(legacy) == originals, "legacy mods still untouched after move");

  Fixture old(directory / "old journal", platform);
  auto prior = Value(old.Prepare());
  prior.lease.reset();
  Write(old.env.install_dir / "mods/custom/data", "newly eligible old mod");
  // A completed v1 journal from before this extension has no mods source IDs.
  const auto journal = old.Paths().data_root / ".narutorise-migration/state.json";
  auto state = nlohmann::json::parse(Read(journal));
  auto& completed = state["completed"];
  for (auto it = completed.begin(); it != completed.end();) {
    if (nlohmann::json::parse(it->get<std::string>())["kind"] == "mods") it = completed.erase(it);
    else ++it;
  }
  Write(journal, state.dump());
  auto upgraded = Value(old.Prepare());
  Check(Read(upgraded.paths.mods_dir / "custom/data") == "newly eligible old mod", "old completed journal imports newly supported mods");

  Fixture conflict(directory / "conflict", platform);
  Write(conflict.env.install_dir / "mods/mod/data", "original");
  Write(conflict.Paths().mods_dir / "mod/data", "divergent");
  auto refused = conflict.Prepare();
  Check(!refused && refused.error.find("conflict") != std::string::npos,
        "divergent mod blocks preparation");
  Check(Read(conflict.env.install_dir / "mods/mod/data") == "original" &&
            Read(conflict.Paths().mods_dir / "mod/data") == "divergent", "both divergent copies preserved");
  Write(conflict.Paths().mods_dir / "mod/data", "original");
  Check(static_cast<bool>(conflict.Prepare()), "identical content permits recovery");

  for (bool environment_override : {false, true}) {
    Fixture isolated(directory / (environment_override ? "env" : "cli"), platform);
    isolated.env.appimage = platform == Platform::kLinux ? Utf8(isolated.root / "external/Game.AppImage") : "";
    Write(isolated.env.install_dir / "mods/personal/data", "private install mod");
    Write(isolated.root / "external/mods/personal/data", "private external mod");
    Write(isolated.env.home_dir / ".cache/narutorise/appimage/old/.ready", "ready");
    Write(isolated.env.home_dir / ".cache/narutorise/appimage/old/mods/personal/data", "private cached mod");
    Write(isolated.env.install_dir / "defaults/mods/pt-br/texts.json", "factory isolated v1");
    isolated.options.documents = []() -> fs::path { throw std::runtime_error("personal discovery forbidden"); };
    const auto selected = isolated.root / Path("isolado 日本語 espaços");
    Overrides cli;
    if (environment_override) isolated.env.data_root = Utf8(selected);
    else cli.data_root = Utf8(selected);
    auto prepared = Value(isolated.Prepare(cli));
    Check(prepared.paths.mods_dir == selected / "mods" &&
              Read(prepared.paths.mods_dir / "pt-br/texts.json") == "factory isolated v1" &&
              !fs::exists(prepared.paths.mods_dir / "personal"), "isolated override copies only curated seed");
    Write(prepared.paths.mods_dir / "pt-br/texts.json", "custom isolated");
    Write(isolated.env.install_dir / "defaults/mods/pt-br/texts.json", "factory isolated v2");
    prepared.lease.reset();
    prepared = Value(isolated.Prepare(cli));
    Check(Read(prepared.paths.mods_dir / "pt-br/texts.json") == "custom isolated", "isolated seed update preserves customization");
  }
  if (platform == Platform::kLinux) {
    Fixture cached(directory / "AppRun mods", platform);
    cached.env.appimage = Utf8(cached.root / Path("external 日本語/Game.AppImage"));
    const auto deployment = cached.env.home_dir / ".cache/narutorise/appimage/old";
    Write(deployment / ".ready", "ready");
    Write(deployment / "mods/community/payload", "cached customization");
    Write(fs::path(std::u8string(cached.env.appimage.begin(), cached.env.appimage.end())).parent_path() /
              "mods/external/payload", "external customization");
    auto prepared = Value(cached.Prepare());
    Check(Read(prepared.paths.mods_dir / "community/payload") == "cached customization" &&
              Read(prepared.paths.mods_dir / "external/payload") == "external customization", "AppRun cache and external mods imported");
  }
}
void BundledRemount(const fs::path& directory) {
  for (bool during_config : {false, true}) {
    Fixture f(directory / (during_config ? "config" : "seed"), Platform::kLinux);
    f.env.appimage = Utf8(f.root / "external/Game.AppImage");
    const auto seed = f.env.install_dir / "defaults/mods/pt-br";
    Write(seed / "mod.toml", "immutable manifest");
    Write(seed / "texts.json", "immutable texts");
    f.options.checkpoint = [during_config](std::string_view event, const fs::path& path) {
      if (event == "published" && path.filename() == (during_config ? "narutorise.toml" : "texts.json"))
        throw std::runtime_error("AppImage disappears after interruption");
    };
    Check(!f.Prepare(), "interrupted immutable resource cannot start runtime");
    const auto old_install = f.env.install_dir;
    const auto archive = f.root / "unmounted package backup";
    fs::rename(old_install, archive);
    f.env.install_dir = f.root / Path("new mount 日本語");
    fs::copy(archive, f.env.install_dir, fs::copy_options::recursive);
    f.options.checkpoint = {};
    const auto resource = f.env.install_dir / (during_config ? "defaults/narutorise.toml" : "defaults/mods/pt-br/texts.json");
    const auto bytes = Read(resource);
    Write(resource, during_config ? "renderer = 'changed'\n" : "changed seed");
    auto refused = f.Prepare();
    Check(!refused && refused.error.find("Bundled resources changed") != std::string::npos,
          "changed bundled resource refuses remount recovery and preserves transaction");
    Write(resource, bytes);
    auto resumed = Value(f.Prepare());
    Check(Read(resumed.paths.mods_dir / "pt-br/texts.json") == "immutable texts" &&
              Read(resumed.paths.mods_dir / "pt-br/mod.toml") == "immutable manifest",
          "identical bundled resources resume across AppImage mount changes");
  }
}
void SuccessAndRepeat(const fs::path& directory) {
  Fixture f(directory);
  GuestTree(f.Guest());
  Write(f.Guest() / "unknown.txt", "leave here");
  Write(f.Guest() / "cache/rebuild.bin", "cache");
  Write(f.Guest() / "launch.marker", "not a save");
  const std::string config = "# preserve exact text\nlauncher_language = 'pt'\nunknown = [1, 2]\n[controls.port1]\ncustom = 'keep'\n";
  Write(f.env.install_dir / "narutorise.toml", config);
  Write(f.env.install_dir / "logs/narutorise.log", "historical log");
  Write(f.env.install_dir / "dlc/pack.stfs", "package");
  Write(f.env.install_dir / "dlc/.installed", "old marker");
  Write(f.env.install_dir / "dlc/.stfs_stage/incomplete", "skip");
  const auto before = Snapshot(f.Guest()), install_before = Snapshot(f.env.install_dir);
  auto prepared = Value(f.Prepare());
  auto paths = prepared.paths;
  for (const auto& [path, bytes] : before) {
    if (path == "unknown.txt" || path == "launch.marker" || *path.begin() == "cache") continue;
    Check(Read(paths.data_root / path) == bytes, "guest bytes and case preserved: " + Utf8(path));
  }
  Check(fs::is_directory(paths.data_root / "E000000000000003/555307E5/empty"), "empty directory preserved");
  Check(Read(paths.config_path) == config, "TOML including comments and unknown keys copied byte-for-byte");
  Check(Read(paths.log_dir / "legacy/install/narutorise.log") == "historical log", "logs are namespaced");
  Check(Read(paths.dlc_dir / ".installed") == "old marker", "staging marker copied without modifying source");
  Check(!fs::exists(paths.dlc_dir / ".stfs_stage") && !fs::exists(paths.data_root / "unknown.txt"), "unknown and incomplete files excluded");
  Check(prepared.diagnostics.size() >= 4, "skipped files recorded");
  Check(before == Snapshot(f.Guest()), "all guest originals remain identical");
  for (const auto& [path, bytes] : install_before)
    Check(Read(f.env.install_dir / path) == bytes, "all installation originals remain identical");
}

void CachedDlc(const fs::path& directory) {
  Fixture f(directory, Platform::kLinux);
  f.env.appimage = Utf8(f.root / "release/Naruto Rise.AppImage");
  const auto cache = f.env.home_dir / ".cache/narutorise/appimage";
  Write(cache / "build-a/.ready", "old build");
  Write(cache / "build-b/.ready", "old build");
  Write(cache / "build-a/dlc/pack", "same package");
  Write(cache / "build-b/dlc/pack", "same package");
  Write(cache / "build-b/dlc/other", "other package");
  auto before = Snapshot(cache);
  auto prepared = Value(f.Prepare());
  Check(Read(prepared.paths.dlc_dir / "pack") == "same package" &&
        Read(prepared.paths.dlc_dir / "other") == "other package", "all historical payload staging imported");
  Check(Snapshot(cache) == before, "cached staging originals preserved");
  prepared.lease.reset();
  Write(cache / "build-a/dlc/pack", "stale changed copy");
  prepared = Value(f.Prepare());
  Check(Read(prepared.paths.dlc_dir / "pack") == "same package", "completed cache not reimported");
  Fixture conflict(directory / "conflict", Platform::kLinux);
  conflict.env.appimage = Utf8(conflict.root / "game.AppImage");
  const auto c = conflict.env.home_dir / ".cache/narutorise/appimage";
  Write(c / "a/.ready", "ready"); Write(c / "b/.ready", "ready");
  Write(c / "a/dlc/pack", "version a"); Write(c / "b/dlc/pack", "version b");
  auto failed = conflict.Prepare();
  Check(!failed, "conflicting packages across old builds block guest startup");
}

void ConfigSnapshotChanges(const fs::path& directory) {
  Fixture legacy(directory / "legacy");
  GuestTree(legacy.Guest());
  const auto source = legacy.env.install_dir / "narutorise.toml";
  Write(source, "unknown = 'original'\n");
  legacy.options.checkpoint = [&](std::string_view event, const fs::path&) {
    if (event == "configured") Write(source, "user_data_root = 'selected guest'\n");
  };
  auto failed = legacy.Prepare();
  Check(!failed && failed.error.find("Configuration changed") != std::string::npos,
        "configuration changed after root resolution cannot publish a mixed snapshot");
  Check(!fs::exists(legacy.Paths().config_path), "changed legacy config is not committed with stale guest routing");
  legacy.options.checkpoint = {};
  auto retried = Value(legacy.Prepare());
  Check(retried.paths.user_data_root == legacy.env.install_dir / "selected guest" &&
        !fs::exists(retried.paths.data_root / "E000000000000001"), "retry resolves the new explicit guest setting before choosing import sources");
  Fixture active(directory / "active");
  auto current = Value(active.Prepare());
  const auto config = current.paths.config_path;
  current.lease.reset();
  active.options.checkpoint = [&](std::string_view event, const fs::path&) {
    if (event == "configured") Write(config, "cache_root = 'changed cache'\n");
  };
  Check(!active.Prepare(), "manual active config mutation also blocks a stale resolved snapshot");
  active.options.checkpoint = {};
  auto resumed = Value(active.Prepare());
  Check(resumed.paths.cache_root == active.env.install_dir / "changed cache", "active config retry publishes current storage settings");
}

void Selections(const fs::path& directory) {
  Fixture f(directory / "windows");
  GuestTree(f.Guest());
  auto active = Value(f.Prepare());
  const auto original = active.paths.data_root;
  const auto descriptor = active.paths.user_descriptor;
  const auto target = f.root / Path("new location 日本語");
  auto same = Value(RequestLocation(active, f.env, Mode::kDefault));
  Check(!same.pending_source, "same-root selection needs no copy");
  const auto before = Read(descriptor);
  Check(!RequestLocation(active, f.env, Mode::kCustom, original / "child"), "descendant target rejected");
  Check(!RequestLocation(active, f.env, Mode::kCustom, original.parent_path()), "ancestor target rejected");
  Check(!RequestLocation(active, f.env, Mode::kCustom), "empty custom choice rejected");
  Check(Read(descriptor) == before && active.paths.data_root == original, "invalid selection preserves descriptor and active snapshot");
  Write(f.root / "not a folder", "keep file");
  Check(!RequestLocation(active, f.env, Mode::kCustom, f.root / "not a folder"), "non-directory destination rejected");
  auto other_cli = Overrides{}; other_cli.data_root = Utf8(target);
  auto occupied = Value(f.Prepare(other_cli));
  Check(!RequestLocation(active, f.env, Mode::kCustom, target), "another writer blocks selection");
  Check(Read(descriptor) == before, "locked destination leaves selection unchanged");
  occupied.lease.reset();
  // An existing destination configuration is preserved by migration policy.
  fs::remove(target / "narutorise.toml");
  auto planned = Value(RequestLocation(active, f.env, Mode::kCustom, target));
  Check(planned.pending_source == original && !fs::exists(planned.config_path), "selection persists intent without copying live config");
  Check(active.paths.data_root == original, "old writers keep the original snapshot");
  auto selected = Value(ResolvePaths(f.env));
  Check(selected.data_root == target && selected.pending_source == original, "next process resolves the pending selection");
  Check(!PrepareData(selected, f.env, {}, f.options), "next process refuses a still-active source writer");
  const auto save = fs::path("E000000000000001/555307E5/00000001/PlayerSave1.ninjasave/SaveData");
  Write(original / save, "save made after requesting selection");
  Write(original / "narutorise.toml", "unknown_after_selection = 42\n");
  active.lease.reset();
  auto moved = Value(PrepareData(selected, f.env, {}, f.options));
  Check(Read(target / save) == "save made after requesting selection", "restart copies the final source state, not a live snapshot");
  Check(Read(target / "narutorise.toml") == "unknown_after_selection = 42\n", "config edits before exit arrive at the selected location");
  Check(!nlohmann::json::parse(Read(descriptor)).contains("pending_source") && !moved.paths.pending_source,
        "successful activation clears the durable pending marker");
  Write(target / save, "newer save at the active destination");
  Write(f.Guest() / save, "stale Documents save");
  moved.lease.reset();
  // Keep the original bytes as a backup, while proving they are no longer
  // required for subsequent boot after the completed descriptor activation.
  const auto backup = f.root / "original backup";
  Check(original.is_absolute() && backup.is_absolute() &&
        !original.lexically_relative(f.root).empty(), "fixture move stays inside the isolated task directory");
  fs::rename(original, backup);
  auto repeated = Value(f.Prepare());
  Check(Read(target / save) == "newer save at the active destination", "inherited completion prevents stale personal reimport");
  Check(Read(backup / save) == "save made after requesting selection", "original backup remains intact");
  Fixture activation(directory / "activation");
  GuestTree(activation.Guest());
  auto old_active = Value(activation.Prepare());
  const auto old_root = old_active.paths.data_root;
  Value(RequestLocation(old_active, activation.env, Mode::kCustom, activation.root / "destination"));
  old_active.lease.reset();
  activation.options.checkpoint = [](std::string_view event, const fs::path&) {
    if (event == "committed") throw std::runtime_error("simulate interruption before descriptor activation");
  };
  Check(!activation.Prepare(), "interruption after durable completion does not publish a usable snapshot");
  const auto retained = activation.root / "retained original";
  Check(old_root.is_absolute() && retained.is_absolute() &&
        !old_root.lexically_relative(activation.root).empty(), "recovery fixture move stays within its isolated root");
  fs::rename(old_root, retained);
  activation.options.checkpoint = {};
  auto recovered = Value(activation.Prepare());
  Check(Read(recovered.paths.data_root / save) == Read(retained / save) && !recovered.paths.pending_source,
        "descriptor activation resumes from committed journal without requiring the source again");
  Fixture isolated(directory / "override");
  Overrides cli; cli.data_root = Utf8(isolated.root / "session");
  auto session = Value(isolated.Prepare(cli));
  Check(!RequestLocation(session, isolated.env, Mode::kCustom, isolated.root / "blocked"), "general session override blocks persistent selection");
  Check(!fs::exists(session.paths.portable_descriptor) && !fs::exists(isolated.root / "blocked"), "blocked override creates no descriptor or destination");
  Fixture linux_case(directory / "linux", Platform::kLinux);
  auto standard = Value(linux_case.Prepare());
  auto portable = Value(RequestLocation(standard, linux_case.env, Mode::kPortable));
  Check(portable.selected_descriptor == standard.paths.portable_descriptor &&
        portable.data_root == standard.paths.distribution_dir / "data", "portable preference stays with the distribution");
  standard.lease.reset();
  auto portable_boot = Value(linux_case.Prepare());
  Check(portable_boot.paths.mode == Mode::kPortable, "portable mode activates on subsequent preparation");
  auto restored = Value(RequestLocation(portable_boot, linux_case.env, Mode::kDefault));
  Check(restored.selected_descriptor == portable_boot.paths.portable_descriptor &&
        restored.data_root == linux_case.Guest(), "restore replaces the higher-priority portable descriptor");
}

void Repeat(const fs::path& directory) {
  Fixture f(directory);
  GuestTree(f.Guest());
  auto before = Snapshot(f.Guest());
  auto first = Value(f.Prepare());
  auto paths = first.paths;
  first.lease.reset();
  const auto save = fs::path("E000000000000001/555307E5/00000001/PlayerSave1.ninjasave/SaveData");
  Write(paths.data_root / save, "new active save");
  Write(f.Guest() / save, "older application changed it");
  auto second = Value(f.Prepare());
  Check(Read(paths.data_root / save) == "new active save", "completed source never reimported");
  Check(Read(f.Guest() / save) == "older application changed it", "repeat never changes source");
  Check(Read(paths.config_path) == Read(f.env.install_dir / "defaults/narutorise.toml"), "defaults initialized once");
  second.lease.reset();
  Write(f.env.install_dir / "defaults/narutorise.toml", "renderer='xenos'\n");
  auto third = Value(f.Prepare());
  Check(Read(paths.config_path) == "renderer = 'native'\n", "new distribution defaults do not reset preferences");
  Check(!before.empty(), "source fixture contains guest content");

  Fixture unavailable(directory / "offline Documents");
  unavailable.options.documents = [&] { return unavailable.root / "disconnected volume/Documents"; };
  auto failed = unavailable.Prepare();
  Check(!failed && failed.error.find("Documents is unavailable") != std::string::npos,
        "unavailable redirected Documents must not silently start with empty saves");

  Fixture portable(directory / "portable");
  GuestTree(portable.Guest());
  auto original = Value(portable.Prepare());
  original.lease.reset();
  auto relocated = portable.root / "relocated installation";
  Check(relocated.parent_path() == portable.root && portable.env.install_dir.parent_path() == portable.root,
        "both directory move targets are within this test's isolated fixture");
  fs::rename(portable.env.install_dir, relocated);
  portable.env.install_dir = relocated;
  auto reopened = Value(portable.Prepare());
  Check(Read(reopened.paths.data_root / save) == std::string("save\0bytes", 10), "completed portable data survives moving the ZIP folder");
}

void ConfigAndOverrides(const fs::path& directory) {
  Fixture f(directory / "custom");
  auto custom_config = "user_data_root = '../custom guest'\ncache_root = 'custom cache'\ndlc_source_path = ''\nunknown = 42\n";
  Write(f.env.install_dir / "narutorise.toml", custom_config);
  f.options.documents = []() -> fs::path { throw std::runtime_error("personal guest discovery forbidden"); };
  f.options.legacy_install_dir = directory / "old installation";
  auto prepared = Value(f.Prepare());
  Check(prepared.paths.user_data_root == directory / "custom guest", "historical relative guest base retained exactly");
  Check(prepared.paths.cache_root == f.options.legacy_install_dir / "custom cache", "historical relative cache base retained");
  Check(!fs::exists(prepared.paths.user_data_root), "custom guest stays in-place without traversal or creation");
  Check(Read(prepared.paths.config_path) == custom_config, "custom and disabled DLC source strings unchanged");
  auto paths = prepared.paths;
  prepared.lease.reset();
  prepared = Value(f.Prepare());
  Check(prepared.config_base == f.options.legacy_install_dir, "historical base persisted across restarts");
  prepared.lease.reset();
  Overrides cli;
  cli.user_data_root = "CLI guest";
  cli.cache_root = "CLI cache";
  f.env.user_data_root = "env guest";
  auto explicit_guest = Value(f.Prepare(cli));
  Check(explicit_guest.paths.user_data_root == f.env.initial_cwd / "CLI guest", "guest CLI beats env and TOML");
  Check(explicit_guest.paths.cache_root == f.env.initial_cwd / "CLI cache", "cache CLI precedence and cwd");

  Fixture isolated(directory / "isolated");
  Write(isolated.env.install_dir / "narutorise.toml", "invalid = [");
  isolated.options.documents = []() -> fs::path { throw std::runtime_error("personal discovery forbidden"); };
  cli = {};
  cli.data_root = Utf8(directory / Path("isolated root 日本語"));
  auto session = Value(isolated.Prepare(cli));
  Check(Read(session.paths.config_path) == "renderer = 'native'\n", "general session override skips legacy config and guest");
  Check(session.paths.session_override, "session isolation remains visible");

  Fixture active(directory / "active");
  auto active_paths = active.Paths();
  Write(active_paths.config_path, "user_data_root='custom'\nunknown='active'\n");
  Write(active.env.install_dir / "narutorise.toml", "invalid = [");
  auto chosen = Value(active.Prepare());
  Check(Read(chosen.paths.config_path).find("active") != std::string::npos, "active TOML wins without parsing broken legacy");
  chosen.lease.reset();
  Write(active_paths.config_path, "invalid = [");
  auto invalid = active.Prepare();
  Check(!invalid && invalid.error.find("Invalid TOML") != std::string::npos, "invalid active TOML blocks preparation");
  Check(Read(active_paths.config_path) == "invalid = [", "invalid active TOML never overwritten");

  Fixture broken(directory / "broken legacy");
  Write(broken.env.install_dir / "narutorise.toml", "invalid = [");
  Check(!broken.Prepare(), "invalid imported TOML blocks before publishing");
  Check(!fs::exists(broken.Paths().config_path), "invalid imported TOML not published");
}

void LinuxAndPending(const fs::path& directory) {
  Fixture f(directory / "linux", Platform::kLinux);
  GuestTree(f.Guest());
  Write(f.env.home_dir / ".config/narutorise/narutorise.toml", "unknown='XDG'\n");
  Write(f.env.install_dir / "narutorise.toml", "invalid=[");
  Write(f.env.home_dir / ".local/state/narutorise/logs/old.log", "xdg log");
  auto before = Snapshot(f.Guest());
  auto prepared = Value(f.Prepare());
  Check(Read(prepared.paths.config_path) == "unknown='XDG'\n", "AppRun config has priority over installed TOML");
  Check(Read(prepared.paths.log_dir / "legacy/xdg/old.log") == "xdg log", "AppRun logs imported");
  for (const auto& [path, bytes] : before) Check(Read(f.Guest() / path) == bytes, "in-place XDG guest left unchanged");
  Check(!fs::exists(prepared.paths.data_root / ".narutorise-migration/0.bin"), "completed stages cleaned");
  auto source = prepared.paths.data_root;
  auto old_paths = prepared.paths;
  auto paths = f.Paths();
  paths.data_root = directory / "pending portable";
  paths.config_path = paths.data_root / "narutorise.toml";
  paths.log_dir = paths.data_root / "logs";
  paths.dlc_dir = paths.data_root / "dlc";
  paths.pending_source = source;
  Check(!PrepareData(paths, f.env, {}, f.options), "pending copy cannot run while former root is leased");
  prepared.lease.reset();
  auto moved = Value(PrepareData(paths, f.env, {}, f.options));
  Check(Read(moved.paths.config_path) == "unknown='XDG'\n", "pending selection imports config");
  for (const auto& [path, bytes] : before) Check(Read(paths.data_root / path) == bytes, "pending selection preserves all guest content");
  Check(Read(source / "narutorise.toml") == "unknown='XDG'\n", "pending copy retains source config");
  moved.lease.reset();
  paths.pending_source = paths.data_root;
  Check(!PrepareData(paths, f.env, {}, f.options), "same pending source rejected");
  paths.pending_source = old_paths.data_root;
  paths.data_root = source / "descendant";
  Check(!PrepareData(paths, f.env, {}, f.options), "ancestor pending source rejected");
}

void Interruptions(const fs::path& directory) {
  for (const std::string event : {"inventoried", "staged", "published", "verified"}) {
    Fixture f(directory / event);
    GuestTree(f.Guest());
    auto before = Snapshot(f.Guest());
    bool stopped = false;
    f.options.checkpoint = [&](std::string_view current, const fs::path&) {
      if (current == event && !stopped) { stopped = true; throw std::runtime_error("injected interruption"); }
    };
    auto interrupted = f.Prepare();
    Check(!interrupted && stopped, "interruption cannot return a usable snapshot: " + event);
    Check(Snapshot(f.Guest()) == before, "interruption preserves originals");
    f.options.checkpoint = {};
    auto resumed = Value(f.Prepare());
    for (const auto& [path, bytes] : before) Check(Read(resumed.paths.data_root / path) == bytes, "resume finishes complete tree");
    auto state = nlohmann::json::parse(Read(resumed.paths.data_root / ".narutorise-migration/state.json"));
    Check(!state.contains("transaction"), "completion recorded only after verified publication");
  }
}

void HistoricalAppRun(const fs::path& directory) {
  auto cache = [](Fixture& f, const std::string& id) {
    const auto old = f.env.home_dir / ".cache/narutorise/appimage" / id;
    Write(old / ".ready", "");
    fs::create_symlink(f.env.home_dir / ".config/narutorise/narutorise.toml", old / "narutorise.toml");
    return old;
  };
  Fixture known(directory / "known", Platform::kLinux);
  known.env.appimage = Utf8(known.root / "Naruto.AppImage");
  Write(known.env.home_dir / ".config/narutorise/narutorise.toml", "user_data_root = 'custom guest'\n");
  auto old = cache(known, "old-build");
  auto prepared = Value(known.Prepare());
  Check(prepared.paths.user_data_root == old / "custom guest", "AppRun relative guest uses the historical payload base");
  prepared.lease.reset();
  cache(known, "another-build");
  prepared = Value(known.Prepare());
  Check(prepared.config_base == old, "historical AppRun base persisted independently of later cache entries");

  Fixture ambiguous(directory / "ambiguous", Platform::kLinux);
  ambiguous.env.appimage = Utf8(ambiguous.root / "Naruto.AppImage");
  Write(ambiguous.env.home_dir / ".config/narutorise/narutorise.toml", "user_data_root = 'custom guest'\n");
  cache(ambiguous, "build-a");
  cache(ambiguous, "build-b");
  auto result = ambiguous.Prepare();
  Check(!result && result.error.find("historical AppRun") != std::string::npos, "ambiguous old bases block instead of choosing empty saves");
  Check(!fs::exists(ambiguous.Paths().config_path), "ambiguous config remains unimported");
}

void ConflictsAndChanges(const fs::path& directory) {
  Fixture conflict(directory / "conflict");
  GuestTree(conflict.Guest());
  const auto save = fs::path("E000000000000001/555307E5/00000001/PlayerSave1.ninjasave/SaveData");
  auto paths = conflict.Paths();
  Write(paths.data_root / save, "different destination");
  auto before = Snapshot(conflict.Guest());
  auto result = conflict.Prepare();
  Check(!result && result.error.find("Migration conflict") != std::string::npos, "divergent save blocks all publication");
  Check(Read(paths.data_root / save) == "different destination" && before == Snapshot(conflict.Guest()), "both conflict versions preserved");
  Check(!fs::exists(paths.config_path), "preflight conflict prevents even config publication");
  Write(paths.data_root / save, before.at(save));
  auto identical = Value(conflict.Prepare());
  Check(Read(identical.paths.data_root / save) == before.at(save), "identical destination recognized on retry");

  Fixture changing(directory / "source changed");
  GuestTree(changing.Guest());
  bool changed = false;
  changing.options.checkpoint = [&](std::string_view event, const fs::path& path) {
    if (event == "staged" && path == changing.Guest() / save && !changed) {
      changed = true;
      Write(path, std::string(10, 'x')); // Same size: byte comparison must detect it.
    }
  };
  result = changing.Prepare();
  Check(!result && changed && result.error.find("changed") != std::string::npos, "same-size mutation during copy detected");
  changing.options.checkpoint = {};
  Check(!changing.Prepare(), "changed source remains blocked across retries");
  Write(changing.Guest() / save, std::string("save\0bytes", 10));
  auto recovered = Value(changing.Prepare());
  Check(Read(recovered.paths.data_root / save) == std::string("save\0bytes", 10), "restored source resumes safely");

  Fixture full(directory / "disk full");
  GuestTree(full.Guest());
  full.options.checkpoint = [](std::string_view event, const fs::path&) {
    if (event == "publishing") throw std::system_error(std::make_error_code(std::errc::no_space_on_device));
  };
  Check(!full.Prepare(), "simulated full disk cannot permit boot");
  full.options.checkpoint = {};
  Check(static_cast<bool>(full.Prepare()), "disk failure can be retried");

  Fixture before_copy(directory / "changed before staging");
  GuestTree(before_copy.Guest());
  before_copy.options.checkpoint = [&](std::string_view event, const fs::path&) {
    if (event == "inventoried") Write(before_copy.Guest() / save, std::string(10, 'y'));
  };
  Check(!before_copy.Prepare(), "persisted fingerprints detect same-size changes before staging");
  before_copy.options.checkpoint = {};
  Write(before_copy.Guest() / save, std::string("save\0bytes", 10));
  Check(static_cast<bool>(before_copy.Prepare()), "restored inventory can resume after early source mutation");
}

void LinksAndInvalidState(const fs::path& directory) {
  Fixture f(directory / "link");
  GuestTree(f.Guest());
  auto outside = directory / "outside.txt";
  Write(outside, "outside untouched");
  auto link = f.Guest() / "E000000000000001/escape";
  fs::create_symlink(outside, link);
  Check(!f.Prepare(), "nested symlink rejected without following it");
  Check(Read(outside) == "outside untouched", "outside source unchanged");
  fs::remove(link);
  Fixture destination(directory / "destination link");
  GuestTree(destination.Guest());
  fs::create_directory_symlink(destination.Guest(), destination.Paths().data_root);
  Check(!destination.Prepare(), "destination symlink rejected");
  fs::remove(destination.Paths().data_root);
  fs::create_symlink(directory / "missing", link);
  Check(!f.Prepare(), "broken symlink rejected");
  fs::remove(link);
  // Storage reached through a link above it (Wine's Z:\ drive, /home linked to
  // /var/home) is accepted; links inside the copied tree stay refused.
  fs::create_directories(directory / "volume");
  fs::create_directory_symlink(directory / "volume", directory / "volume link");
  for (const auto platform : {Platform::kWindows, Platform::kLinux}) {
    const auto name = platform == Platform::kWindows ? "windows" : "linux";
    Fixture through(directory / "volume link" / name, platform);
    GuestTree(through.Guest());
    // Legacy mods are copied on both platforms; the Linux guest root is the data root.
    Write(through.env.install_dir / "mods/community/data", "mod");
    const auto escape = through.env.install_dir / "mods/community/escape";
    fs::create_symlink(outside, escape);
    Check(!through.Prepare(), "nested symlink below a linked ancestor rejected");
    fs::remove(escape);
    const auto linked = Value(through.Prepare());
    Check(Read(linked.paths.data_root / "E000000000000001/555307E5/00000001/PlayerSave1.ninjasave/SaveData") ==
              std::string("save\0bytes", 10), "storage below a linked ancestor prepared");
  }
  auto ok = Value(f.Prepare());
  auto state = ok.paths.data_root / ".narutorise-migration/state.json";
  ok.lease.reset();
  Write(state, "{broken");
  Check(!f.Prepare(), "corrupted journal blocks rather than resetting migration");
  Check(Read(state) == "{broken", "corrupted journal preserved");
  Fixture permissions(directory / "permissions");
  GuestTree(permissions.Guest());
  auto paths = permissions.Paths();
  auto metadata = paths.data_root / ".narutorise-migration";
  fs::create_directories(metadata);
#ifdef _WIN32
  DWORD original_size = 0;
  GetFileSecurityW(metadata.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &original_size);
  std::vector<char> original(original_size);
  Check(GetFileSecurityW(metadata.c_str(), DACL_SECURITY_INFORMATION,
                         reinterpret_cast<PSECURITY_DESCRIPTOR>(original.data()), original_size, &original_size),
        "read fixture ACL");
  PSECURITY_DESCRIPTOR denied = nullptr;
  Check(ConvertStringSecurityDescriptorToSecurityDescriptorW(
      L"D:(D;;GW;;;WD)(A;;GA;;;WD)", SDDL_REVISION_1, &denied, nullptr), "prepare denied-write ACL");
  const bool applied = SetFileSecurityW(metadata.c_str(), DACL_SECURITY_INFORMATION, denied);
  LocalFree(denied);
  Check(applied, "deny writes to fixture metadata");
  auto failed = permissions.Prepare();
  Check(SetFileSecurityW(metadata.c_str(), DACL_SECURITY_INFORMATION,
                         reinterpret_cast<PSECURITY_DESCRIPTOR>(original.data())), "restore fixture ACL");
#else
  Check(getuid() != 0, "permission test must run as ordinary user");
  Check(chmod(metadata.c_str(), 0500) == 0, "restrict fixture metadata");
  auto failed = permissions.Prepare();
  Check(chmod(metadata.c_str(), 0700) == 0, "restore fixture metadata permissions");
#endif
  Check(!failed && !fs::exists(paths.config_path), "real permission denial prevents publication");
  Check(static_cast<bool>(permissions.Prepare()), "permission failure can resume");
}

int RunChild(std::string_view mode, const fs::path& root) {
#ifdef _WIN32
  std::wstring command = L"\"" + executable.wstring() + L"\" " + Path(mode).wstring() + L" \"" + root.wstring() + L"\"";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  Check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &startup, &process) != 0, "spawn child");
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD result = 0;
  GetExitCodeProcess(process.hProcess, &result);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return static_cast<int>(result);
#else
  auto child = fork();
  Check(child >= 0, "fork child");
  if (!child) {
    auto argument = std::string(mode);
    execl(executable.c_str(), executable.c_str(), argument.c_str(), root.c_str(), nullptr);
    _exit(99);
  }
  int result = 0;
  Check(waitpid(child, &result, 0) == child && WIFEXITED(result), "child exits normally");
  return WEXITSTATUS(result);
#endif
}
void Processes(const fs::path& directory) {
  Fixture f(directory / "locks");
  auto prepared = Value(f.Prepare());
  Check(!f.Prepare(), "second lease refused in same process");
  Check(RunChild("--locked", f.root) == 0, "second process refused while writer lease held");
  prepared.lease.reset();
  Check(static_cast<bool>(f.Prepare()), "lease releases normally");
  for (const std::string mode : {"--crash", "--crash-published"}) {
    Fixture crash(directory / mode);
    GuestTree(crash.Guest());
    auto before = Snapshot(crash.Guest());
    Check(RunChild(mode, crash.root) == 73, "process actually terminated without destructors");
    auto resumed = Value(crash.Prepare());
    for (const auto& [path, bytes] : before) Check(Read(resumed.paths.data_root / path) == bytes, "OS releases crashed lock and journal resumes");
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 && argc != 3) return 2;
  try {
    executable = fs::absolute(Path(argv[0]));
    if (argc == 3) {
      Fixture f(fs::absolute(Path(argv[2])));
      if (std::string_view(argv[1]) == "--locked") return f.Prepare() ? 90 : 0;
      const auto mode = std::string_view(argv[1]);
      if (mode != "--crash" && mode != "--crash-published") return 91;
      f.options.checkpoint = [mode](std::string_view event, const fs::path&) {
        if (event == (mode == "--crash" ? "staged" : "published")) {
#ifdef _WIN32
          ExitProcess(73);
#else
          _exit(73);
#endif
        }
      };
      return f.Prepare() ? 92 : 93;
    }
    auto root = fs::absolute(Path(argv[1])).lexically_normal();
    Check(!fs::exists(root), "fixture must be a new isolated directory");
    fs::create_directories(root);
    SuccessAndRepeat(root / "success");
    Mods(root / "mods windows", Platform::kWindows);
    Mods(root / "mods linux", Platform::kLinux);
    BundledRemount(root / "AppImage interrupted remount");
    Repeat(root / "repeat");
    Selections(root / "selections");
    ConfigSnapshotChanges(root / "config snapshot");
    CachedDlc(root / "cached dlc");
    ConfigAndOverrides(root / "config");
    LinuxAndPending(root / "linux pending");
    Interruptions(root / "interruptions");
    HistoricalAppRun(root / "historical AppRun");
    ConflictsAndChanges(root / "conflicts");
    LinksAndInvalidState(root / "links");
    Processes(root / "processes");
    std::cout << "PASS: " << checks << " checks; legacy copy, TOML, overrides, resume, conflicts, links, locks, process death\n";
  } catch (const std::exception& error) {
    std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
    return 1;
  }
}
