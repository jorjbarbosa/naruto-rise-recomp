// narutorise - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <utility>
#include <fmt/format.h>
#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/rex_app.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xcontent.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>

#include "crash_report.h"
#include "game_image.h"
#include "graphics_device.h"
#include "input_replay.h"
#include "intro_skip.h"
#include "launch_arguments.h"
#include "menu/setup_gate.h"
#include "menu/game_choice.h"
#include "menu/settings.h"
#include "menu/dlc.h"
#include "menu/input_gate.h"
#include "menu/shortcut.h"
#include "menu/update_checker.h"
#include "menu/ui.h"
#include "mods/translation_mod.h"
#include "ultrawide.h"
#include "window_title.h"
#ifdef NARUTORISE_NATIVE_RENDERER_BOOTSTRAP
#include "gpu/host/app.h"
#include "gpu/settings.h"
#include "gpu/ui/immediate_drawer.h"
#include "gpu/ui/layer.h"
#include "gpu/ui/screen.h"
#include "gpu/common/shader_activity.h"
#include "wine.h"
#endif

REXCVAR_DEFINE_BOOL(show_fps_overlay, false, "narutorise/UI",
                    "Show FPS and frametime overlay (Settings > Advanced)");

REXCVAR_DEFINE_BOOL(setup, false, "narutorise/UI",
                    "Open the setup screen before the game (also Shift held at the start)");
REXCVAR_DEFINE_BOOL(show_startup_menu, true, "narutorise/UI",
                    "Show the initial menu before the game (Settings > Advanced)");
REXCVAR_DEFINE_BOOL(check_for_updates, true, "narutorise/UI", "Notify when a new release is available");
REXCVAR_DEFINE_UINT32(restart_wait_pid, 0, "narutorise/Internal", "Wait for the previous process during a settings restart");

REXCVAR_DEFINE_STRING(dlc_source_path, "dlc", "Content",
                      "Folder scanned for DLC packages to auto-install on launch. "
                      "If empty or missing, the game runs without DLC.");

// Guest frame stats source for the F1/F3 overlays. The command processor
// measures the frame time on every guest swap (XE_SWAP) and snapshots it in
// the perf counter registry; game speed follows this rate, not the host
// window present rate. Returns frame_count == 0 when the SDK was built
// without REXGLUE_ENABLE_PERF_COUNTERS (guest rate unavailable).
inline rex::ui::FrameStats ReadGuestFrameStats() {
  const int64_t fps = rex::perf::GetSnapshotCounter(rex::perf::CounterId::kFps);
  const int64_t ft_us =
      rex::perf::GetSnapshotCounter(rex::perf::CounterId::kFrameTimeUs);
  rex::ui::FrameStats stats;
  if (fps > 0) {
    stats.fps = static_cast<double>(fps);
    stats.frame_time_ms = static_cast<double>(ft_us) / 1000.0;
    stats.frame_count = 1;  // valid data this frame
  }
  return stats;
}

class FullscreenShortcuts : public rex::ui::WindowInputListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    using rex::ui::VirtualKey;
    if (e.virtual_key() != VirtualKey::kF11 &&
        !(e.virtual_key() == VirtualKey::kReturn && e.is_alt_pressed())) {
      return;
    }

    // Consume repeats too, so holding the shortcut neither toggles repeatedly
    // nor sends Enter to the game's keyboard controller or an overlay.
    e.set_handled(true);
    if (!e.prev_state() && e.target()) {
      // ReXApp's fullscreen callback applies the change on this UI thread.
      rex::cvar::SetFlagByName("fullscreen",
                              e.target()->IsFullscreen() ? "false" : "true");
    }
  }
};

class NarutoriseApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  void OnPreSetup(rex::RuntimeConfig& config) override {
    runtime_config_ = &config;
    // Configured native ports and optional input recording/replay for test
    // boots. Legacy renderer paths retain the SDK's default assignment.
    config.input_factory = CreateInputSystem;
#ifdef NARUTORISE_NATIVE_RENDERER_BOOTSTRAP
    gpu::host::Configure(config);
#endif
    // The Xenos renderer (no graphics system of the native one): its D3D12
    // adapter by the graphics_device name.
    if (!config.graphics) SelectXenosAdapter();
    narutorise::menu::input_gate_enabled = NativeRenderer();
  }

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<NarutoriseApp>(new NarutoriseApp(ctx, "narutorise",
        PPCImageConfig));
  }

  // A folder of extracted files or a disc image file, which the SDK mounts in
  // place (Runtime::SetupVfs).
  static bool IsGameDataRoot(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec) || std::filesystem::is_regular_file(path, ec);
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
#ifdef _WIN32
    if (const auto parent = REXCVAR_GET(restart_wait_pid); parent && parent != GetCurrentProcessId()) {
      if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, parent)) {
        WaitForSingleObject(process, INFINITE); CloseHandle(process);
      }
    }
#endif
    // Only the command line has set the root by now: the configuration loads after this.
    command_line_root_ = !paths.game_data_root.empty();
    const auto exe_dir = rex::filesystem::GetExecutableFolder();
    // CLI/environment strings are UTF-8. The SDK's initial narrow path
    // conversion uses the Windows code page; resolve the effective root here.
    const auto explicit_user_root = REXCVAR_GET(user_data_root);
    const auto explicit_cache_root = REXCVAR_GET(cache_root);
    if (!explicit_user_root.empty()) {
      paths.user_data_root = std::filesystem::absolute(std::filesystem::u8path(explicit_user_root)).lexically_normal();
      if (explicit_cache_root.empty()) paths.cache_root = paths.user_data_root / "cache";
    }
    if (!explicit_cache_root.empty())
      paths.cache_root = std::filesystem::absolute(std::filesystem::u8path(explicit_cache_root)).lexically_normal();
    const std::filesystem::path toml_path = exe_dir / "narutorise.toml";
    std::optional<toml::table> toml_tbl;
    if (std::filesystem::exists(toml_path)) {
      try {
        std::ifstream input(toml_path, std::ios::binary);
        toml_tbl = toml::parse(input);
      } catch (...) {
      }
    }
    if (toml_tbl) {
      Localization::SetLanguageCode((*toml_tbl)["launcher_language"].value_or(std::string("en")));
      // The retired launcher passed this TOML setting to the SDK as a CLI
      // argument. Resolve it here before the SDK constructs the runtime paths.
      // CLI/environment overrides are already in the cvars and keep priority.
      if (REXCVAR_GET(user_data_root).empty()) {
        const auto configured = (*toml_tbl)["user_data_root"].value_or(std::string());
        if (!configured.empty()) {
          auto root = std::filesystem::u8path(configured);
          paths.user_data_root = root.is_relative() ? exe_dir / root : root;
          if (REXCVAR_GET(cache_root).empty()) paths.cache_root = paths.user_data_root / "cache";
        }
      }
    }
    if (paths.game_data_root.empty() || !IsGameDataRoot(paths.game_data_root)) {
      if (toml_tbl) {
        if (auto v = (*toml_tbl)["custom_game_root"].value<std::string>()) {
          std::error_code ec;
          configured_root_ = !v->empty();
          if (!v->empty() && IsGameDataRoot(std::filesystem::u8path(*v))) {
            paths.game_data_root = std::filesystem::canonical(std::filesystem::u8path(*v), ec);
            REXLOG_INFO("[narutorise] Using custom_game_root from TOML: {}",
                        paths.game_data_root.string());
          }
        }
      }

      if (paths.game_data_root.empty()) {
        paths.game_data_root = narutorise::game_image::FindNearbyGame(exe_dir);
        if (!paths.game_data_root.empty()) {
          REXLOG_INFO("[narutorise] Auto-detected game_data_root: {}", paths.game_data_root.string());
        }
      }
    }

    ConfigureUpdateDataRoot(paths);
  }

  // Which game the data root holds, once the log exists: the recompiled code
  // only runs with the default.xex it was generated from (game_image.h).
  void OnPostInitLogging() override {
    if (game_data_root().empty()) return;
    LogGameData(game_data_root());
  }
  void LogGameData(const std::filesystem::path& root) {
    using narutorise::game_image::Status;
    game_identity_ = narutorise::game_image::Identify(root);
    const auto line = fmt::format("[narutorise] Game data: {} ({}); Title ID {:08X}, default.xex {}: {}",
                                  root.string(), game_identity_.disc_image ? "disc image" : "folder",
                                  game_identity_.title_id, game_identity_.xex_digest,
                                  narutorise::game_image::StatusName(game_identity_.status));
    if (game_identity_.status == Status::kReady)
      REXLOG_INFO("{}", line);
    else
      REXLOG_WARN("{}", line);
  }

  // The initial menu follows the saved startup preference and recovery rules
  // (menu/setup_gate.h); it shows on the window before the
  // runtime exists, and its Play resumes the boot. The native mode only: with
  // Xenos the game boots as configured.
  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)> resume) override {
    SetupUi();
    if (!NativeRenderer()) return defaults;
    // The launch marker: there from here until the game's first frame, so a
    // start that dies before it (the engine of a card or API that fails) opens
    // the setup screen next time. The boots that name their game have none.
    if (!command_line_root_ && !user_data_root().empty()) {
      launch_marker_ = user_data_root() / "launch.marker";
      std::error_code ec;
      start_failed_ = std::filesystem::exists(launch_marker_, ec);
      std::filesystem::create_directories(user_data_root(), ec);
      std::ofstream(launch_marker_) << "started\n";
    }
    namespace menu = narutorise::menu;
    menu::SetupFacts facts;
    facts.command_line_root = command_line_root_;
    facts.configured = configured_root_;
    facts.found = !defaults.game_data_root.empty();
    facts.status = game_identity_.status;
    facts.start_failed = start_failed_;
    facts.requested = REXCVAR_GET(setup) || menu::SetupKeyHeld();
    facts.show_startup_menu = REXCVAR_GET(show_startup_menu);
    const menu::SetupReason reason = menu::SetupReasonFor(facts);
    if (reason == menu::SetupReason::kNone || !OpenSetupScreen(reason, defaults, std::move(resume))) {
      ArmFirstPresentation();
      return defaults;
    }
    return std::nullopt;
  }

  void ConfigureUpdateDataRoot(rex::PathConfig& paths) {
    if (paths.game_data_root.empty()) return;

    const auto exe_dir = rex::filesystem::GetExecutableFolder();
    const std::filesystem::path toml_path = exe_dir / "narutorise.toml";

    // 1. Explicit TOML setting.
    if (std::filesystem::exists(toml_path)) {
      try {
        std::ifstream input(toml_path, std::ios::binary);
        auto tbl = toml::parse(input);
        if (auto v = tbl["update_data_root"].value<std::string>()) {
          std::error_code ec;
          if (!v->empty() && std::filesystem::is_directory(std::filesystem::u8path(*v), ec)) {
            paths.update_data_root = std::filesystem::canonical(std::filesystem::u8path(*v), ec);
            REXLOG_INFO("[narutorise] Using update_data_root from TOML: {}",
                        paths.update_data_root.string());
            return;
          }
        }
      } catch (...) {
      }
    }

    // 2. Explicit cvar / CLI setting.
    std::string update_cvar = REXCVAR_GET(update_data_root);
    if (!update_cvar.empty()) {
      std::error_code ec;
      auto candidate = std::filesystem::path(update_cvar);
      if (std::filesystem::is_directory(candidate, ec)) {
        paths.update_data_root = std::filesystem::canonical(candidate, ec);
        REXLOG_INFO("[narutorise] Using update_data_root from cvar: {}",
                    paths.update_data_root.string());
        return;
      }
    }

    // 3. Auto-detect title_update/ next to the game data or one level above.
    const std::filesystem::path candidates[] = {
        paths.game_data_root / "title_update",
        paths.game_data_root.parent_path() / "title_update",
        paths.game_data_root / "update",
        paths.game_data_root.parent_path() / "update",
    };
    for (const auto& candidate : candidates) {
      std::error_code ec;
      if (std::filesystem::is_directory(candidate, ec)) {
        paths.update_data_root = std::filesystem::canonical(candidate, ec);
        REXLOG_INFO("[narutorise] Auto-detected update_data_root: {}",
                    paths.update_data_root.string());
        return;
      }
    }
  }

  void OnPostLoadXexImage() override {
    ApplyUltrawide();
  }

  // The anamorphic projection relies on the SDK presenter stretching the 16:9
  // image to the window. The native renderer keeps the 16:9 image with bars,
  // so the game stays at 16:9 there.
  void ApplyUltrawide() {
#ifdef NARUTORISE_NATIVE_RENDERER_BOOTSTRAP
    if (gpu::host::Enabled()) {
      if (rex::cvar::Query<double>("ultrawide_target_aspect") > 0.0) {
        REXLOG_INFO("[narutorise] ultrawide_target_aspect is not used by the native renderer (16:9)");
      }
      return;
    }
#endif
    ApplyUltrawidePatch(runtime()->virtual_membase());
  }

  void OnPostSetup() override {
    narutorise::translation::Initialize(rex::filesystem::GetExecutableFolder());
    narutorise::crash_report::Install();  // After the SDK's exception handler.
    // The game's name and version in place of the SDK's name and build stamp.
    if (window()) window()->SetTitle(narutorise::WindowTitle());
    if (window()) {
      // Window shortcuts take priority over overlays and keyboard emulation.
      window()->AddInputListener(&fullscreen_shortcuts_,
                                 std::numeric_limits<size_t>::max());
    }
    RestoreXenosAdapter();  // The provider exists by now.
    SeedShaderStorage();
    AutoInstallDlc();

    // Guest frame rate (measured per guest swap by the command processor) for
    // the SDK debug overlay (F3).
    SetGuestFrameStats(&ReadGuestFrameStats);

    // Encerramento limpo via Alt+F4
    rex::ui::RegisterBind("bind_exit_game", "Alt+F4", "Exit Game", [this]() {
      RequestExit();
    });

    if (NativeRenderer()) {
      game_running_ = true;
      UpdateMenuShortcut();
      char hint[512];
      const auto& options = menu_settings_.menu_options();
      std::string chord;
      for (char c : options.chord) chord += c == '+' ? " + " : std::string(1, c);
      std::snprintf(hint, sizeof(hint), narutorise::menu::Text("shortcut-format"), options.key.c_str(), chord.c_str());
      shortcut_hint_ = hint;
    } else {
      rex::ui::RegisterBind("bind_fps_overlay", "F1", "Toggle FPS overlay", [this]() {
        SetFpsVisible(!ui_.FpsVisible());
        rex::cvar::SetFlagByName("show_fps_overlay", ui_.FpsVisible() ? "true" : "false");
      });
    }

    // Callback para reaplicar o aspect ratio em tempo real caso a cvar mude
    rex::cvar::RegisterChangeCallback(
        "ultrawide_target_aspect", [this](std::string_view, std::string_view) {
          if (runtime()) {
            ApplyUltrawide();
          }
        });
  }

  bool OnWindowCloseRequested() override {
    if (NativeRenderer()) {
      RequestExit();
      return false; // SDL must release its event-watch lock before joining guest threads.
    }
    return true;
  }

  void RequestExit() {
    if (!NativeRenderer()) {
      if (window()) window()->RequestClose();
      return;
    }
    if (closing_.exchange(true)) return;
#ifdef NARUTORISE_NATIVE_RENDERER_BOOTSTRAP
    gpu::ui::UiLayer().SetFrameRequest({});
    gpu::ui::UiLayer().SetFirstPresentCallback({});
#endif
    app_context().CallInUIThreadDeferred([this] { FinishExit(); });
  }

  void FinishExit() {
    // OnClosing hard-exits before normal OnShutdown teardown. Stop haptics
    // here, and block new guest motor commands while shutdown joins it.
    narutorise::menu::input_blocked = true;
    narutorise::controls::Hardware().StopAllRumble();
    menu_dlc_.Wait(); // Finish filesystem copies before the process can exit.
    // Closed on the setup screen: the start ends cleanly there.
    if (setup_resume_) {
      setup_resume_ = nullptr;
      CloseSetupScreen();
      RemoveLaunchMarker();
    }
#ifdef NARUTORISE_NATIVE_RENDERER_BOOTSTRAP
    // The native renderer closes in order (guest threads joined, device
    // owner, resources and engine released) before OnClosing hard-exits.
    gpu::host::CloseRequested();
#endif
    narutorise::controls::Hardware().StopAllRumble();
    if (window()) window()->RequestClose();
  }

  void OnShutdown() override {
    game_running_ = false;
    narutorise::menu::input_blocked = false;
    CloseSetupScreen();
    if (window()) window()->RemoveInputListener(&ui_);
    if (window()) window()->RemoveInputListener(&fullscreen_shortcuts_);
    rex::cvar::UnregisterChangeCallbacks("ultrawide_target_aspect");
    rex::ui::UnregisterBind("bind_exit_game");
    rex::ui::UnregisterBind("bind_fps_overlay");
#ifdef NARUTORISE_NATIVE_RENDERER_BOOTSTRAP
    gpu::ui::UiLayer().SetFrameRequest({});  // No Swap asks for a UI frame from here on.
    gpu::ui::UiLayer().SetFirstPresentCallback({});
#endif
    ui_.Shutdown();
  }

 private:
  static bool IsHexString(std::string_view s) {
    return !s.empty() &&
           s.find_first_not_of("0123456789abcdefABCDEF") == std::string_view::npos;
  }

  // Mirrors already-extracted (loose) content into the user data tree.
  // STFS packages are skipped: they must be installed from their source
  // location with InstallContent, which derives the destination folder from
  // the package file name (a package copied here could collide with its own
  // extraction destination).
  static bool MirrorDlcDirectory(const std::filesystem::path& source,
                                 const std::filesystem::path& destination) {
    std::error_code ec;
    std::filesystem::create_directories(destination, ec);
    bool ok = true;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             source, std::filesystem::directory_options::skip_permission_denied,
             ec)) {
      std::error_code entry_ec;
      if (!entry.is_regular_file(entry_ec) || IsStfsFile(entry.path())) {
        continue;
      }
      auto rel = std::filesystem::relative(entry.path(), source, entry_ec);
      if (entry_ec) {
        ok = false;
        continue;
      }
      const auto dst = destination / rel;
      std::filesystem::create_directories(dst.parent_path(), entry_ec);
      std::filesystem::copy_file(entry.path(), dst,
                                 std::filesystem::copy_options::update_existing,
                                 entry_ec);
      if (entry_ec) ok = false;
    }
    return ok;
  }

  static std::string TruncatePackageName(std::string_view name, size_t max_bytes) {
    if (name.size() <= max_bytes) return std::string(name);
    size_t len = max_bytes;
    while (len > 0 && (static_cast<uint8_t>(name[len]) & 0xC0) == 0x80) {
      --len;
    }
    return std::string(name.substr(0, len));
  }

  static bool IsStfsFile(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return false;
    auto size = std::filesystem::file_size(path, ec);
    if (ec || size < 0x344) return false;

    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::uint32_t magic = 0;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    // Packages start with the ASCII magic 'CON ', 'LIVE' or 'PIRS'. Those
    // bytes are big-endian in the file, so a native (little-endian) uint32
    // read yields the byte-swapped values below.
    return magic == 0x204E4F43u || magic == 0x4556494Cu || magic == 0x53524950u;
  }

  // Collects STFS package files inside a directory tree.
  static std::vector<std::filesystem::path> CollectStfsFiles(
      const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied,
             ec)) {
      std::error_code entry_ec;
      if (entry.is_regular_file(entry_ec) && IsStfsFile(entry.path())) {
        files.push_back(entry.path());
      }
    }
    return files;
  }

  // Path of the .header sidecar the ContentManager uses for a marketplace
  // package, given the package root (<user_root>/<xuid>/<title>/00000002).
  // Mirrors ContentManager::ResolvePackageHeaderPath.
  static std::filesystem::path DlcHeaderPath(
      const std::filesystem::path& dlc_root, std::string_view file_name) {
    return dlc_root.parent_path() / "Headers" / "00000002" /
           (std::string(file_name) + ".header");
  }

  // Installs DLC content on startup.
  //
  // Phase 1 scans the dlc/ source folder (dlc_source_path):
  //   * Folders following the Xbox 360 content layout (named after a Title ID
  //     or a XUID) have their loose files mirrored into the user data tree,
  //     while any STFS packages inside are installed from their source
  //     location with ContentManager::InstallContent.
  //   * Other entries are scanned recursively for STFS packages.
  //
  // Phase 2 fixes marketplace content already present in the user data tree
  // (placed there manually or by older versions): extracts stray STFS files,
  // renames folders that don't fit XCONTENT_AGGREGATE_DATA::file_name_raw
  // (42 bytes) and writes missing .header sidecars with a full license mask.
  void AutoInstallDlc() {
    auto* kernel_state = runtime() ? runtime()->kernel_state() : nullptr;
    auto* content_manager = kernel_state ? kernel_state->content_manager() : nullptr;
    if (!content_manager) {
      REXLOG_WARN("[narutorise] DLC auto-install skipped: content manager unavailable");
      return;
    }

    const std::string title_id = "555307E5";
    const std::string xuid = "0000000000000000";
    const uint32_t parsed_title_id =
        static_cast<uint32_t>(std::stoul(title_id, nullptr, 16));
    const auto user_root = user_data_root();
    const auto content_root = user_root / xuid;
    const auto dlc_root = content_root / title_id / "00000002";
    std::error_code ec;

    // --- Phase 1: install content from the dlc/ source folder. ---

    std::vector<std::filesystem::path> packages;
    int mirrored_dirs = 0;

    std::string dlc_path_cvar = REXCVAR_GET(dlc_source_path);
    if (dlc_path_cvar.empty()) {
      REXLOG_INFO("[narutorise] DLC auto-install disabled (dlc_source_path is empty)");
    } else {
      std::filesystem::path root =
          rex::filesystem::GetExecutableFolder() / dlc_path_cvar;
      if (!std::filesystem::is_directory(root, ec)) {
        REXLOG_INFO("[narutorise] DLC folder not found ({}); running without DLC",
                    dlc_path_cvar);
      } else {
        // Marker file from older versions: the already-installed check in the
        // install loop below replaces it.
        std::filesystem::remove(root / ".installed", ec);

        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
          const auto filename = entry.path().filename().string();

          if (entry.is_regular_file(ec)) {
            if (IsStfsFile(entry.path())) {
              packages.push_back(entry.path());
            } else {
              REXLOG_WARN("[narutorise] Ignoring non-STFS file in DLC folder: {}",
                          filename);
            }
            continue;
          }
          if (!entry.is_directory(ec)) continue;

          // STFS packages must be installed from their source location:
          // InstallContent derives the destination folder from the package
          // file name, so a package already sitting in the content tree could
          // collide with its own extraction destination.
          auto stfs_files = CollectStfsFiles(entry.path());

          // Title ID folder (e.g. 555307E5) - mirror loose content into
          // <user_root>/0000000000000000/<title_id>.
          if (filename.size() == 8 && IsHexString(filename)) {
            if (MirrorDlcDirectory(entry.path(), content_root / filename)) {
              REXLOG_INFO("[narutorise] Mirrored DLC content directory: {}",
                          filename);
              mirrored_dirs++;
            } else {
              REXLOG_WARN("[narutorise] Failed to mirror DLC directory: {}",
                          filename);
            }
          } else if (filename.size() == 16 && IsHexString(filename)) {
            // XUID folder - mirror loose content into <user_root>/<xuid>.
            if (MirrorDlcDirectory(entry.path(), user_root / filename)) {
              REXLOG_INFO("[narutorise] Mirrored DLC content directory: {}",
                          filename);
              mirrored_dirs++;
            } else {
              REXLOG_WARN("[narutorise] Failed to mirror DLC directory: {}",
                          filename);
            }
          }
          // Other folders are only scanned for STFS packages (collected
          // above).

          packages.insert(packages.end(), stfs_files.begin(), stfs_files.end());
        }

        // Install STFS packages that are not installed yet. A package counts
        // as installed when both its extracted folder and its .header sidecar
        // (written by InstallContent with the license from the package) exist
        // in the user data tree, so this is idempotent across launches and
        // self-heals if the user data is ever wiped.
        int new_packages = 0;
        for (const auto& pkg : packages) {
          std::error_code rel_ec;
          auto rel = std::filesystem::relative(pkg, root, rel_ec).string();
          if (rel.empty()) rel = pkg.filename().string();

          const auto file_name = rex::path_to_utf8(pkg.filename());
          rex::system::xam::XCONTENT_AGGREGATE_DATA probe{};
          probe.content_type = rex::system::XContentType::kMarketplaceContent;
          probe.title_id = parsed_title_id;
          probe.set_file_name(file_name);
          rex::system::xam::XCONTENT_AGGREGATE_DATA existing_header{};
          if (content_manager->ContentExists(0, probe) &&
              XSUCCEEDED(content_manager->ReadContentHeaderFile(
                  file_name, 0, parsed_title_id,
                  rex::system::XContentType::kMarketplaceContent,
                  existing_header))) {
            continue;
          }

          REXLOG_INFO("[narutorise] Installing DLC package: {}", rel);
          auto result = content_manager->InstallContent(pkg);
          if (XSUCCEEDED(result)) {
            new_packages++;
          } else {
            REXLOG_WARN("[narutorise] DLC install failed for {}: 0x{:08X}", rel,
                        static_cast<uint32_t>(result));
          }
        }

        if (packages.empty() && mirrored_dirs == 0) {
          REXLOG_INFO("[narutorise] DLC folder empty ({}); running without DLC",
                      dlc_path_cvar);
        } else {
          REXLOG_INFO(
              "[narutorise] DLC auto-install: {} new package(s), {} mirrored dir(s)",
              new_packages, mirrored_dirs);
        }
      }
    }

    // --- Phase 2: fix marketplace content in the user data tree. ---

    if (!std::filesystem::is_directory(dlc_root, ec)) {
      REXLOG_INFO("[narutorise] No installed DLC content to fix");
      return;
    }

    int fixed = 0;

    // Snapshot entries first: installing/removing content mutates the tree.
    std::vector<std::filesystem::path> installed_entries;
    for (const auto& pkg : std::filesystem::directory_iterator(dlc_root, ec)) {
      installed_entries.push_back(pkg.path());
    }

    for (const auto& package_path : installed_entries) {
      std::error_code entry_ec;

      if (std::filesystem::is_regular_file(package_path, entry_ec)) {
        // Stray STFS file directly in the package root (e.g. mirrored there
        // by an older version). Stage it outside the content tree first, as
        // InstallContent would try to extract it onto its own path.
        if (!IsStfsFile(package_path)) continue;
        auto staged = user_root / ".stfs_stage" / package_path.filename();
        std::filesystem::create_directories(staged.parent_path(), entry_ec);
        std::filesystem::copy_file(package_path, staged,
                                   std::filesystem::copy_options::skip_existing,
                                   entry_ec);
        if (entry_ec) {
          REXLOG_WARN("[narutorise] Failed to stage STFS package '{}': {}",
                      rex::path_to_utf8(package_path.filename()),
                      entry_ec.message());
          continue;
        }
        REXLOG_INFO("[narutorise] Installing staged STFS package: {}",
                    rex::path_to_utf8(package_path.filename()));
        auto result = content_manager->InstallContent(staged);
        if (XSUCCEEDED(result)) {
          std::error_code remove_ec;
          std::filesystem::remove(package_path, remove_ec);
          std::filesystem::remove(staged, remove_ec);
          fixed++;
        } else {
          REXLOG_WARN("[narutorise] STFS install failed: 0x{:08X}",
                      static_cast<uint32_t>(result));
        }
        continue;
      }

      if (!std::filesystem::is_directory(package_path, entry_ec)) continue;

      auto package_name = rex::path_to_utf8(package_path.filename());

      // Wrapper folder containing STFS packages: let the SDK extract them
      // (this writes the header with the real license) and drop the wrapper.
      std::vector<std::filesystem::path> stfs_files;
      for (const auto& sub :
           std::filesystem::directory_iterator(package_path, entry_ec)) {
        if (sub.is_regular_file(entry_ec) && IsStfsFile(sub.path())) {
          stfs_files.push_back(sub.path());
        }
      }
      if (!stfs_files.empty()) {
        bool all_installed = true;
        for (const auto& stfs : stfs_files) {
          REXLOG_INFO("[narutorise] Installing STFS package: {}",
                      rex::path_to_utf8(stfs.filename()));
          auto result = content_manager->InstallContent(stfs);
          if (XSUCCEEDED(result)) {
            REXLOG_INFO("[narutorise] STFS install succeeded");
            fixed++;
          } else {
            REXLOG_WARN("[narutorise] STFS install failed: 0x{:08X}",
                        static_cast<uint32_t>(result));
            all_installed = false;
          }
        }
        // Only drop the wrapper folder (and any header sidecar previously
        // written for it) once every package inside was extracted
        // successfully - the wrapper may hold the only copy of the package.
        if (all_installed) {
          std::error_code remove_ec;
          std::filesystem::remove_all(package_path, remove_ec);
          std::filesystem::remove(DlcHeaderPath(dlc_root, package_name),
                                  remove_ec);
        } else {
          REXLOG_WARN("[narutorise] Keeping DLC folder '{}' for retry",
                      package_name);
        }
        continue;
      }

      // Loose content: make sure folder name fits in file_name_raw (42 bytes)
      // and a header exists.
      auto short_name = TruncatePackageName(package_name, 42);
      if (short_name != package_name) {
        auto new_path = dlc_root / short_name;
        std::error_code rename_ec;
        std::filesystem::rename(package_path, new_path, rename_ec);
        if (rename_ec) {
          REXLOG_WARN("[narutorise] Failed to rename DLC folder '{}': {}",
                      package_name, rename_ec.message());
          continue;
        }
        // Drop the header sidecar written for the old (too long) name.
        std::filesystem::remove(DlcHeaderPath(dlc_root, package_name),
                                rename_ec);
        REXLOG_INFO("[narutorise] Renamed DLC folder to '{}'", short_name);
      }

      // Don't clobber headers written by InstallContent (they carry the real
      // license mask extracted from the STFS package).
      rex::system::xam::XCONTENT_AGGREGATE_DATA existing{};
      if (XSUCCEEDED(content_manager->ReadContentHeaderFile(
              short_name, 0, parsed_title_id,
              rex::system::XContentType::kMarketplaceContent, existing))) {
        continue;
      }

      rex::system::xam::XCONTENT_AGGREGATE_DATA data{};
      data.device_id = 0;
      data.content_type = rex::system::XContentType::kMarketplaceContent;
      data.title_id = parsed_title_id;
      data.xuid = 0;
      data.set_display_name(rex::path_to_utf16(std::filesystem::path(short_name)));
      data.set_file_name(short_name);

      auto result = content_manager->WriteContentHeaderFile(0, data, 0xFFFFFFFF);
      if (XSUCCEEDED(result)) {
        REXLOG_INFO("[narutorise] Wrote DLC header for '{}'", short_name);
        fixed++;
      } else {
        REXLOG_WARN("[narutorise] Failed to write DLC header for '{}': 0x{:08X}",
                    short_name, static_cast<uint32_t>(result));
      }
    }

    // Diagnostic: ask ContentManager to list marketplace content.
    auto listed = content_manager->ListContent(
        0, 0, rex::system::XContentType::kMarketplaceContent, parsed_title_id);
    REXLOG_INFO("[narutorise] ContentManager lists {} marketplace package(s)",
                listed.size());
    for (const auto& item : listed) {
      REXLOG_INFO("[narutorise]   - file_name='{}'", item.file_name());
    }

    REXLOG_INFO("[narutorise] DLC fix-up complete: {} item(s) fixed", fixed);
  }

  void SeedShaderStorage() {
    const std::filesystem::path bundled =
        rex::filesystem::GetExecutableFolder() / "shader_cache";
    std::error_code ec;
    if (!std::filesystem::is_directory(bundled, ec) ||
        runtime()->cache_root().empty()) {
      return;
    }
    const std::filesystem::path shareable =
        runtime()->cache_root() / "shaders" / "shareable";
    std::filesystem::create_directories(shareable, ec);
    uint32_t seeded = 0;
    for (const auto& entry :
         std::filesystem::directory_iterator(bundled, ec)) {
      if (!entry.is_regular_file()) continue;
      const std::filesystem::path dst = shareable / entry.path().filename();
      if (std::filesystem::exists(dst, ec)) continue;
      if (std::filesystem::copy_file(entry.path(), dst, ec) && !ec) {
        ++seeded;
      }
    }
    if (seeded) {
      REXLOG_INFO("[narutorise] Seeded {} shader cache file(s) into {}", seeded,
                  shareable.string());
    }
  }

  // The Xenos presenter (none in the native mode). Its graphics system stays in
  // the configuration of OnPreSetup until the runtime takes it.
  rex::ui::Presenter* XenosPresenter() const {
    rex::system::IGraphicsSystem* graphics = runtime() ? runtime()->graphics_system()
                                             : runtime_config_ ? runtime_config_->graphics.get() : nullptr;
    return graphics ? graphics->presenter() : nullptr;
  }

  // The UI of the menus (menu/ui.h) through the renderer's immediate drawing,
  // from before the runtime on (the setup screen): the SDK's with Xenos (the
  // presenter draws the Ui), the UI layer's in the native mode, where each
  // Swap (each Present of the setup screen) asks the UI thread for the next
  // frame while a document is shown. The frame rate counts the Swaps there,
  // and is the guest's with Xenos (the presenter's paints without it).
  void SetupUi() {
    menu_settings_.Initialize(rex::filesystem::GetExecutableFolder(), NativeRenderer());
    rex::ui::ImmediateDrawer* drawer = immediate_drawer();
    rex::ui::Presenter* presenter = XenosPresenter();
    if (presenter) {
      ui_.SetRateSource([](double& fps, double& ms) {
        const rex::ui::FrameStats stats = ReadGuestFrameStats();
        fps = stats.fps;
        ms = stats.frame_time_ms;
        return stats.frame_count > 0;
      });
    }
#ifdef NARUTORISE_NATIVE_RENDERER_BOOTSTRAP
    // The native mode keeps a graphics system without a presenter in the runtime.
    if (gpu::host::Enabled()) {
      presenter = nullptr;
      native_drawer_ = std::make_unique<gpu::ui::ImmediateDrawer>(gpu::ui::UiLayer());
      drawer = native_drawer_.get();
      gpu::ui::UiLayer().SetFrameRequest([this] {  // The Swap's thread.
        if (closing_.load()) return;
        ui_.CountFrame();
        if ((!ui_.Shown() && !ui_.GameMenuEnabled()) || ui_draw_pending_.exchange(true)) return;
        app_context().CallInUIThread([this] {
          ui_draw_pending_ = false;
          if (closing_.load()) return;
          const bool visible = ui_.Shown();
          if (!visible) ui_.PollInput();
          if (visible || ui_.Shown()) DrawNativeUi();
        });
      });
    }
#endif
    if (!drawer || (!presenter && !native_drawer_) || !ui_.Initialize(*drawer, presenter)) {
      REXLOG_WARN("[narutorise] UI unavailable: no immediate drawer for the renderer");
      return;
    }
    // Below the window shortcuts (fullscreen), above the SDK's overlays.
    if (window()) window()->AddInputListener(&ui_, std::numeric_limits<size_t>::max() - 1);
    ui_.SetInputActive([this] { return window() && window()->HasFocus(); });
    if (NativeRenderer()) {
      ui_.SetTick([this] { ServiceMenu(); });
      menu_settings_.SetUtilities([this] { CreateDesktopShortcut(); }, [this] { CheckUpdates(); });
      if (REXCVAR_GET(check_for_updates))
        updates_ = std::make_unique<launcher_updates::Checker>(narutorise::Version(), user_data_root() / "menu-updates.json");
    }
    SetFpsVisible(REXCVAR_GET(show_fps_overlay));
  }

  bool NativeRenderer() const {
#ifdef NARUTORISE_NATIVE_RENDERER_BOOTSTRAP
    return gpu::host::Enabled();
#else
    return false;
#endif
  }

  // The setup screen (menu/setup_screen.h) for reason, on the window, which a
  // temporary engine presents (native mode). Play resumes the boot with paths;
  // false when it cannot show.
  bool OpenSetupScreen(narutorise::menu::SetupReason reason, const rex::PathConfig& paths,
                       std::function<void(rex::PathConfig)> resume) {
    REXLOG_INFO("[narutorise] Setup screen: {}", narutorise::menu::SetupReasonName(reason));
#ifdef NARUTORISE_NATIVE_RENDERER_BOOTSTRAP
    if (!OpenNativeScreen()) return false;
#endif
    // The choices run inside RmlUi's event handling; the screen closes after it
    // (deferred: CallInUIThread runs at once on the UI thread).
    const auto later = [this](void (NarutoriseApp::*step)()) {
      return [this, step] { app_context().CallInUIThreadDeferred([this, step] { (this->*step)(); }); };
    };
    narutorise::menu::SetupScreen::State state{reason, game_identity_, narutorise::Version(), paths.game_data_root};
    narutorise::menu::SetupScreen::Actions actions;
    actions.play = later(&NarutoriseApp::FinishSetup);
    actions.quit = later(&NarutoriseApp::QuitFromSetup);
    actions.choose_game = later(&NarutoriseApp::ChooseGame);
    actions.settings = later(&NarutoriseApp::OpenSettings);
    actions.dlc = later(&NarutoriseApp::OpenDlc);
    if (!ui_.ShowSetup(state, std::move(actions))) {
      REXLOG_WARN("[narutorise] Setup screen unavailable; the game boots as configured");
      CloseSetupScreen();
      return false;
    }
    setup_paths_ = paths;
    setup_reason_ = reason;
    setup_resume_ = std::move(resume);
    return true;
  }
#ifdef NARUTORISE_NATIVE_RENDERER_BOOTSTRAP
  // The temporary engine of the setup screen: the configured API and card, or
  // the other API when they fail (the screen is where they change). Under Wine
  // the native renderer runs on Vulkan only.
  bool OpenNativeScreen() {
    const auto& settings = gpu::CurrentSettings();
    gpu::RenderMode mode;
    mode.device = settings.graphics_device;
    mode.api = settings.native_backend == "vulkan" ? gpu::GraphicsApi::kVulkan : gpu::GraphicsApi::kD3D12;
    void* hwnd = window() ? window()->GetNativeWindowHandle() : nullptr;
    std::string evidence;
    if (hwnd && setup_screen_.Open(hwnd, mode, evidence)) return true;
    REXLOG_WARN("[narutorise] Setup screen: the {} engine did not start: {}", settings.native_backend, evidence);
    if (!hwnd || (mode.api == gpu::GraphicsApi::kVulkan && narutorise::WineVersion())) return false;
    mode.api = mode.api == gpu::GraphicsApi::kVulkan ? gpu::GraphicsApi::kD3D12 : gpu::GraphicsApi::kVulkan;
    mode.device.clear();
    if (setup_screen_.Open(hwnd, mode, evidence)) return true;
    REXLOG_WARN("[narutorise] Setup screen: the other engine did not start either: {}", evidence);
    return false;
  }
#endif
  // The screen's engine goes before the game's (native mode); the UI layer
  // keeps the frame of what still shows (the frame rate) for it.
  void CloseSetupScreen() {
    ui_.HideSetup();
#ifdef NARUTORISE_NATIVE_RENDERER_BOOTSTRAP
    if (!setup_screen_.IsOpen()) return;
    if (native_drawer_) DrawNativeUi();
    setup_screen_.Close();
#endif
  }
  void FinishSetup() {
    if (!setup_resume_ || game_choice_.Busy() || game_identity_.status != narutorise::game_image::Status::kReady) return;
    if (!menu_settings_.ApplyBeforeGame()) {
      ui_.ShowDialog(narutorise::menu::Text("settings"), narutorise::menu::Text("save-failed"),
                     {{narutorise::menu::Text("cancel"), {}}});
      return;
    }
    auto resume = std::exchange(setup_resume_, nullptr);
    CloseSetupScreen();
#ifdef NARUTORISE_NATIVE_RENDERER_BOOTSTRAP
    gpu::LoadSettings();
#endif
    if (setup_paths_.game_data_root != game_data_root()) LogGameData(setup_paths_.game_data_root);
    ArmFirstPresentation();
    resume(setup_paths_);
  }
  void OpenSettings() {
#ifdef NARUTORISE_NATIVE_RENDERER_BOOTSTRAP
    if (NativeRenderer()) menu_settings_.SetLiveSettings(
      [](const std::string& key, const GameConfig& config) -> uint64_t {
        auto next = gpu::RequestedSettings();
        if (key == "vsync") next.vsync = config.vsync;
        else if (key == "brightness") next.brightness = float(config.brightness);
        else if (key == "frame_rate") next.frame_rate = config.frame_rate;
        else if (key == "resolution_scale") {
          const float scale = float(config.internal_resolution_scale > 0 ? config.internal_resolution_scale : config.resolution_scale);
          next.resolution_scale = {scale, scale};
        } else if (key == "msaa_samples") next.msaa_samples = config.msaa_samples;
        else if (key == "anisotropic_override") next.anisotropic_override = config.anisotropic_override;
        else if (key == "transparency_antialiasing") next.transparency_antialiasing = config.transparency_antialiasing;
        else if (key == "max_frame_latency") next.max_frame_latency = config.max_frame_latency;
        else if (key == "triple_buffering") next.triple_buffering = config.triple_buffering;
        else if (key == "swap_post_effect") next.swap_post_effect = config.swap_post_effect == "fxaa_extreme"
            ? gpu::SwapPostEffect::kFxaaExtreme : config.swap_post_effect == "fxaa" ? gpu::SwapPostEffect::kFxaa : gpu::SwapPostEffect::kNone;
        else if (key == "present_effect") next.present_effect = config.present_effect == "fsr"
            ? gpu::PresentEffect::kFsr : config.present_effect == "cas" ? gpu::PresentEffect::kCas : gpu::PresentEffect::kBilinear;
        else return 0;
        return gpu::RequestSettings(std::move(next));
      }, [] {
        const auto result = gpu::LastSettingsResult();
        return narutorise::menu::Settings::LiveResult{result.revision, result.applied};
      });
#endif
    menu_settings_.Open(ui_, 0, !setup_resume_, [this] {
      app_context().CallInUIThreadDeferred([this] { ChooseGame(); });
    }, [this](const std::string& key) {
      if (key == "show_fps_overlay") {
        rex::cvar::SetFlagByName(key, menu_settings_.config().show_fps_overlay ? "true" : "false");
        SetFpsVisible(menu_settings_.config().show_fps_overlay);
      } else if (key == "fullscreen") {
        rex::cvar::SetFlagByName(key, menu_settings_.config().fullscreen ? "true" : "false");
      } else if (key == "skip_intro_videos") {
        narutorise::SetSkipIntroVideos(menu_settings_.config().skip_intro_videos);
      } else if (key == "native_backend" || key == "graphics_device") {
        app_context().CallInUIThreadDeferred([this] { OfferSettingsRestart(); });
      } else if (key == "bind_game_menu" || key == "game_menu_chord") {
        if (game_running_) UpdateMenuShortcut();
      }
    }, [this] {
      if (setup_resume_) ui_.RefreshSetup({setup_reason_, game_identity_, narutorise::Version(), setup_paths_.game_data_root});
      else if (ui_.OverlayShown()) OpenGameMenu();
    });
  }
  void OpenDlc() {
    menu_dlc_.Open(ui_, rex::filesystem::GetExecutableFolder(), user_data_root(), window()->GetNativeWindowHandle(),
        [this](std::function<void()> action) { app_context().CallInUIThreadDeferred(std::move(action)); }, [this] {
          if (setup_resume_) ui_.RefreshSetup({setup_reason_, game_identity_, narutorise::Version(), setup_paths_.game_data_root});
        });
  }
  void OfferSettingsRestart() {
    if (closing_.load()) return;
    using narutorise::menu::Text;
    std::string message = Text("changed");
    if (game_running_) message += std::string("\n\n") + Text("exit-confirm");
    ui_.ShowDialog(Text("restart-required"), message, {
        {Text("restart-now"), [this] {
          app_context().CallInUIThreadDeferred([this] { RestartForSettings(); });
        }},
        {Text("later"), {}}});
  }
  void RestartForSettings() {
#ifdef _WIN32
    // Start a fresh process with the saved API/card and the setup gate. The
    // current process closes in its normal order, including its screen engine.
    const auto executable = rex::filesystem::GetExecutableFolder() / L"narutorise.exe";
    if (menu_settings_.config().custom_game_root.empty() && !game_data_root().empty()) {
      std::string error;
      if (!narutorise::menu::SaveGameRoot(executable.parent_path() / "narutorise.toml", game_data_root(), error)) return;
    }
    const std::wstring command = narutorise::launch::QuoteArgument(executable.wstring()) + L" --setup " +
        narutorise::launch::UserDataArgument(user_data_root()) + L" --restart_wait_pid=" + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    auto writable = command;
    if (CreateProcessW(executable.c_str(), writable.data(), nullptr, nullptr, FALSE, 0, nullptr,
                       rex::filesystem::GetExecutableFolder().c_str(), &startup, &process)) {
      REXLOG_INFO("[narutorise] Restarting for saved settings: process {}", process.dwProcessId);
      CloseHandle(process.hThread); CloseHandle(process.hProcess);
      RemoveLaunchMarker();
      RequestExit();
    } else ui_.ShowDialog(narutorise::menu::Text("restart"), narutorise::menu::Text("save-failed"), {{narutorise::menu::Text("cancel"), {}}});
#endif
  }
  void UpdateMenuShortcut() {
    const auto& options = menu_settings_.menu_options();
    ui_.EnableGameMenu(options.key, options.chord, [this] { ToggleGameMenu(); });
  }
  void OpenGameMenu() {
    if (!NativeRenderer() || !game_running_) return;
    ui_.ShowOverlay([this] { OpenSettings(); }, [this] { OpenDlc(); }, [this] {
      using narutorise::menu::Text;
      ui_.ShowDialog(std::string(Text("exit-game")) + "?", Text("exit-confirm"), {
          {Text("exit-game"), [this] { RequestExit(); }},
          {Text("cancel"), {}}});
    }, [this] { CloseGameMenu(); });
    narutorise::menu::input_blocked = ui_.OverlayShown();
    REXLOG_INFO("[narutorise] Game menu opened; guest input neutral");
  }
  void CloseGameMenu() {
    ui_.HideOverlay();
    narutorise::menu::input_blocked = false;
    DrawNativeUi(); // Replace the persistent GPU frame when nothing remains.
    REXLOG_INFO("[narutorise] Game menu closed; guest input resumes after release");
  }
  void ToggleGameMenu() { if (ui_.OverlayShown()) CloseGameMenu(); else OpenGameMenu(); }
  void CreateDesktopShortcut() {
    const bool ok = narutorise::menu::WriteShortcut(narutorise::menu::DesktopShortcut(),
        rex::filesystem::GetExecutableFolder() / "narutorise.exe", user_data_root());
    ui_.Notify(narutorise::menu::Text(ok ? "shortcut-done" : "save-failed"));
  }
  void CheckUpdates() {
    if (!updates_) updates_ = std::make_unique<launcher_updates::Checker>(narutorise::Version(), user_data_root() / "menu-updates.json");
    using narutorise::menu::Text;
    if (updates_->status() == launcher_updates::Status::Available) {
      const auto release = updates_->release();
      char title[512]; std::snprintf(title, sizeof(title), Tr(TextId::UpdatesAvailable), release.version.c_str());
      ui_.ShowDialog(title, Text("update-hint"), {
          {Text("open-release"), [url = release.page_url] { SDL_OpenURL(url.c_str()); }}, {Text("cancel"), {}}});
    } else {
      if (updates_->CanCheck()) updates_->CheckNow();
      manual_update_check_ = true;
      ui_.Notify(Tr(TextId::UpdatesChecking));
    }
  }
  void ServiceMenu() {
    PollGameChoice(); menu_dlc_.Poll();
    menu_settings_.PollLive();
#ifdef NARUTORISE_NATIVE_RENDERER_BOOTSTRAP
    if (shortcut_hint_pending_ && ui_.OverlayShown()) shortcut_hint_pending_ = false;
    if (shortcut_hint_pending_ && std::chrono::steady_clock::now() >= shortcut_hint_due_) {
      shortcut_hint_pending_ = false; ui_.ShowShortcutHint(shortcut_hint_);
    }
    if (game_running_) {
      const auto progress = gpu::CurrentShaderProgress();
      ui_.SetShadersVisible(menu_settings_.menu_options().shaders && gpu::ShaderCompilationVisible(), progress.pending, progress.fraction);
    }
#endif
    if (!updates_) return;
    updates_->Poll();
    if (updates_->status() == launcher_updates::Status::Available && !updates_->IsIgnored() && !update_notified_) {
      update_notified_ = true;
      const auto release = updates_->release();
      char message[512]; std::snprintf(message, sizeof(message), narutorise::menu::Text("update-detail"), release.version.c_str());
      ui_.Notify(message, narutorise::menu::Text("open-release"), [url = release.page_url] { SDL_OpenURL(url.c_str()); }, narutorise::menu::Text("update-available"));
    }
    if (manual_update_check_ && updates_->status() != launcher_updates::Status::Checking) {
      manual_update_check_ = false;
      if (updates_->status() != launcher_updates::Status::Available)
        ui_.Notify(Tr(updates_->status() == launcher_updates::Status::Current ? TextId::UpdatesCurrent : TextId::UpdatesFailed));
    }
  }
  void ChooseGame() {
    if ((!setup_resume_ && !ui_.OverlayShown()) || game_choice_.Busy()) return;
    const auto selected = narutorise::menu::ChooseFile(window()->GetNativeWindowHandle());
    if (!selected.error.empty()) {
      ui_.ShowDialog(narutorise::menu::Text("picker-failed"), selected.error, {{narutorise::menu::Text("cancel"), {}}});
    } else if (!selected.path.empty()) {
      if (!setup_resume_ && !ui_.OverlayShown()) return;
      game_choice_.Start(selected.path);
      game_choice_view_ = ui_.ViewRevision();
      checking_game_ = false;
    }
  }
  void PollGameChoice() {
    if (!game_choice_.Busy()) return;
    if (auto result = game_choice_.Poll()) {
      if (game_choice_view_ != ui_.ViewRevision()) { checking_game_ = false; return; }
      ui_.HideDialog();
      checking_game_ = false;
      using narutorise::game_image::Status;
      if (result->game.status == Status::kReady) {
        std::string error;
        if (!narutorise::menu::SaveGameRoot(rex::filesystem::GetExecutableFolder() / "narutorise.toml", result->root, error)) {
          ui_.ShowDialog(narutorise::menu::Text("save-failed"), error, {{narutorise::menu::Text("cancel"), {}}});
          return;
        }
        if (!setup_resume_) {
          menu_settings_.Reload(); OpenSettings();
          using narutorise::menu::Text;
          ui_.ShowDialog(Text("restart"), Text("changed"), {
              {Text("restart"), [this] { app_context().CallInUIThreadDeferred([this] { RestartForSettings(); }); }},
              {Text("cancel"), {}}});
          return;
        }
        setup_paths_.game_data_root = result->root;
        ConfigureUpdateDataRoot(setup_paths_);
        game_identity_ = result->game;
        configured_root_ = true;
        setup_reason_ = narutorise::menu::SetupReason::kRequested;
        ui_.HidePanel();
        // Reuse the current callbacks and refresh the ready state without
        // replacing the screen's engine or its resume continuation.
        ui_.RefreshSetup({setup_reason_, game_identity_, narutorise::Version(), setup_paths_.game_data_root});
        REXLOG_INFO("[narutorise] Game selected: {}", result->root.string());
      } else {
        using narutorise::menu::Text;
        const char* title = Text(result->game.status == Status::kOtherVersion ? "unsupported" :
                            result->game.status == Status::kIncomplete ? "incomplete" :
                            result->game.status == Status::kUnreadable ? "unreadable" : "unrecognized");
        ui_.ShowDialog(title, Text(result->game.status == Status::kOtherVersion ? "unsupported-detail" :
                                  result->game.status == Status::kIncomplete ? "incomplete-detail" : "game-hint"),
            {{Text("choose-another"), [this] { app_context().CallInUIThreadDeferred([this] { ChooseGame(); }); }}, {Text("cancel"), {}}});
      }
    } else if (!checking_game_ && game_choice_.Slow() && game_choice_view_ == ui_.ViewRevision()) {
      checking_game_ = true;
      ui_.ShowDialog(narutorise::menu::Text("checking"), narutorise::menu::Text("wait"));
    }
  }
  void QuitFromSetup() {
    if (!setup_resume_) return;
    setup_resume_ = nullptr;
    CloseSetupScreen();
    RemoveLaunchMarker();
    RequestExit();
  }
  void RemoveLaunchMarker() {
    std::error_code ec;
    if (!launch_marker_.empty()) std::filesystem::remove(launch_marker_, ec);
  }
  void ArmFirstPresentation() {
#ifdef NARUTORISE_NATIVE_RENDERER_BOOTSTRAP
    gpu::ui::UiLayer().SetFirstPresentCallback([this] { // Engine owner, after a successful game Present.
      if (closing_.load()) return;
      app_context().CallInUIThread([this] {
        if (closing_.load()) return;
        RemoveLaunchMarker();
        if (game_running_ && !shortcut_hint_.empty()) {
          shortcut_hint_pending_ = true;
          shortcut_hint_due_ = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        }
      });
    });
#endif
  }
  void SetFpsVisible(bool visible) {
    ui_.SetFpsVisible(visible);
    // The native UI layer keeps the last frame: hiding draws the empty one.
    if (!visible && native_drawer_ && !ui_.Shown()) DrawNativeUi();
  }
  // A frame of the UI over the native renderer's whole back buffer (the window's client area).
  void DrawNativeUi() {
    if (!window()) return;
    rex::ui::AppUIDrawContext context(window()->GetActualPhysicalWidth(), window()->GetActualPhysicalHeight());
    ui_.Draw(context);
  }

  FullscreenShortcuts fullscreen_shortcuts_;
  std::unique_ptr<rex::ui::ImmediateDrawer> native_drawer_;  // Before ui_: it outlives the Ui.
  narutorise::menu::Ui ui_;
  narutorise::menu::Settings menu_settings_;
  narutorise::menu::Dlc menu_dlc_;
  std::unique_ptr<launcher_updates::Checker> updates_;
  bool game_running_ = false, update_notified_ = false, manual_update_check_ = false;
  std::string shortcut_hint_;
  bool shortcut_hint_pending_ = false;
  std::chrono::steady_clock::time_point shortcut_hint_due_{};
  std::atomic<bool> ui_draw_pending_{false};
  std::atomic<bool> closing_{false};
  rex::RuntimeConfig* runtime_config_ = nullptr;  // ReXApp's, from OnPreSetup on.
  // The game data and the setup screen (menu/setup_gate.h).
  narutorise::game_image::Identity game_identity_;
  bool command_line_root_ = false, configured_root_ = false, start_failed_ = false;
  std::filesystem::path launch_marker_;
  rex::PathConfig setup_paths_;
  narutorise::menu::SetupReason setup_reason_ = narutorise::menu::SetupReason::kNone;
  narutorise::menu::GameChoice game_choice_;
  uint64_t game_choice_view_ = 0;
  bool checking_game_ = false;
  std::function<void(rex::PathConfig)> setup_resume_;  // While the setup screen shows.
#ifdef NARUTORISE_NATIVE_RENDERER_BOOTSTRAP
  gpu::ui::Screen setup_screen_{gpu::ui::UiLayer()};
#endif
};
