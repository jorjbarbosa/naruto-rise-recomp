// narutorise - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <fmt/format.h>
#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/rex_app.h>
#include <rex/system.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xcontent.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>

#include "crash_report.h"
#include "sentry_reporting.h"
#include "diagnostics/incident.h"
#include "menu/folders.h"
#include "data_migration.h"
#include "game_image.h"
#include "graphics_device.h"
#include "input_replay.h"
#include "intro_skip.h"
#include "launch_arguments.h"
#include "menu/setup_gate.h"
#include "menu/game_choice.h"
#include "menu/settings.h"
#include "menu/dlc.h"
#include "menu/content_manager.h"
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

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#endif

REXCVAR_DEFINE_STRING(data_root, "", "narutorise/Storage",
                      "Data folder for this session (--data_root or REX_DATA_ROOT; no automatic legacy import)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(show_fps_overlay, false, "narutorise/UI",
                    "Show FPS and frametime overlay (Settings > Advanced)");

REXCVAR_DEFINE_BOOL(setup, false, "narutorise/UI",
                    "Open the setup screen before the game (also Shift held at the start)");
REXCVAR_DEFINE_BOOL(show_startup_menu, true, "narutorise/UI",
                    "Show the initial menu before the game (Settings > Advanced)");
REXCVAR_DEFINE_BOOL(check_for_updates, true, "narutorise/UI", "Notify when a new release is available");
REXCVAR_DEFINE_BOOL(crash_reporting, false, "narutorise/Reporting", "Allow crash reports and startup timing to be sent to Sentry");
REXCVAR_DEFINE_STRING(sentry_test, "", "narutorise/Internal", "Developer verification: error or crash; requires crash_reporting=true")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(incident_test, "", "narutorise/Internal", "Disposable-process local capture verification; no game data required")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
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

  bool SetupEnvironment() override {
    narutorise::incident::Event("data-and-configuration", "begin");
#ifdef _WIN32
    // A settings restart must release the previous writer's lease first.
    if (const auto parent = REXCVAR_GET(restart_wait_pid); parent && parent != GetCurrentProcessId()) {
      if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, parent)) {
        WaitForSingleObject(process, INFINITE); CloseHandle(process);
      }
    }
#endif
    using namespace narutorise::data;
    try {
      auto environment = CaptureEnvironment(rex::filesystem::GetExecutableFolder());
      if (!environment) return FailDataBootstrap(environment.error);
      Overrides cli;
      for (auto [name, value] : {std::pair{"data_root", &cli.data_root},
                                 std::pair{"user_data_root", &cli.user_data_root},
                                 std::pair{"cache_root", &cli.cache_root}}) {
        if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kCommandLine)
          *value = rex::cvar::GetFlagByName(name);
      }
      auto paths = ResolvePaths(*environment.value, cli);
      if (!paths) return FailDataBootstrap(paths.error);
      auto prepared = PrepareData(*paths.value, *environment.value, cli);
      if (!prepared) return FailDataBootstrap(prepared.error);
      prepared_data_ = std::move(*prepared.value);
      if (const auto source = rex::cvar::GetFlagSource("game_data_root");
          source == rex::cvar::Source::kCommandLine || source == rex::cvar::Source::kEnvironment) {
        const auto game = rex::to_path(REXCVAR_GET(game_data_root));
        if (!game.empty()) restart_game_root_ = (game.is_absolute() ? game : environment.value->initial_cwd / game).lexically_normal();
      }
      auto writable = ValidateWritablePaths(prepared_data_->paths);
      if (!writable) return FailDataBootstrap(writable.error);
      data_environment_ = std::move(*environment.value);
      data_cli_ = cli;
      if (!rex::ReXApp::SetupEnvironment()) {
        narutorise::reporting::Close();
        return false;
      }
      const auto sentry_test = REXCVAR_GET(sentry_test);
      if (const auto test = REXCVAR_GET(incident_test); !test.empty()) {
        narutorise::incident::VerifyIncident(test);
        narutorise::reporting::Close();
        std::_Exit(2);
      }
      if (!sentry_test.empty()) {
        const bool verified = narutorise::reporting::Verify(sentry_test);
        narutorise::reporting::Close();
        if (verified) narutorise::incident::ExpectedExit("sentry-verification");
        rex::FlushLogging();
        std::_Exit(verified ? 0 : 2);
      }
      return true;
    } catch (const std::exception& e) {
      return FailDataBootstrap(e.what());
    }
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    // Only CLI/environment have loaded; the SDK reads the active TOML next.
    command_line_root_ = !paths.game_data_root.empty();
    const auto& active = prepared_data_->paths;
    paths.user_data_root = active.user_data_root;
    paths.cache_root = active.cache_root;
    paths.config_path = active.config_path;
    paths.log_dir = active.log_dir;
    const auto explicit_game = REXCVAR_GET(game_data_root);
    if (!explicit_game.empty()) {
      auto game = rex::to_path(explicit_game);
      paths.game_data_root = (game.is_absolute() ? game : active.initial_cwd / game).lexically_normal();
    }
    const auto& toml_path = active.config_path;
    std::optional<toml::table> toml_tbl;
    std::ifstream input(toml_path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot reopen the active TOML: " + rex::path_to_utf8(toml_path));
    toml_tbl = toml::parse(input);
    if (toml_tbl) {
      Localization::SetLanguageCode((*toml_tbl)["launcher_language"].value_or(std::string("en")));
    }
    if (paths.game_data_root.empty() || !IsGameDataRoot(paths.game_data_root)) {
      if (toml_tbl) {
        if (auto v = (*toml_tbl)["custom_game_root"].value<std::string>()) {
          std::error_code ec;
          configured_root_ = !v->empty();
          if (!v->empty() && IsGameDataRoot(std::filesystem::u8path(*v))) {
            paths.game_data_root = std::filesystem::canonical(std::filesystem::u8path(*v), ec);
            REXLOG_INFO("[narutorise] Using custom_game_root from TOML: {}",
                        rex::path_to_utf8(paths.game_data_root));
          }
        }
      }

      if (paths.game_data_root.empty()) {
        paths.game_data_root = narutorise::game_image::FindNearbyGame(active.distribution_dir);
        if (!paths.game_data_root.empty()) {
          REXLOG_INFO("[narutorise] Auto-detected game_data_root: {}", rex::path_to_utf8(paths.game_data_root));
        }
      }
    }

    ConfigureUpdateDataRoot(paths);
  }

  // Which game the data root holds, once the log exists: the recompiled code
  // only runs with the default.xex it was generated from (game_image.h).
  void OnPostInitLogging() override {
    const auto& active = prepared_data_->paths;
    narutorise::incident::Event("data-and-configuration", "end");
    narutorise::incident::SetMetadata("renderer_requested", rex::cvar::GetFlagByName("renderer"));
    narutorise::incident::SetMetadata("native_backend_requested", rex::cvar::GetFlagByName("native_backend"));
    narutorise::reporting::Initialize(rex::filesystem::GetExecutableFolder(), active.cache_root,
                                    narutorise::Version(), REXCVAR_GET(crash_reporting));
    narutorise::incident::Event("runtime-and-xex", "begin");
    for (const auto* key : {"renderer", "native_backend", "graphics_device", "resolution_scale", "msaa_samples",
                            "frame_rate", "translation_mod", "skip_intro_videos", "log_level"})
      narutorise::incident::SetMetadata(key, rex::cvar::GetFlagByName(key));
    // A failed previous session in this log too: the log is what players attach.
    if (const auto previous = narutorise::incident::Latest(true, true); !previous.empty())
      REXLOG_WARN("[narutorise] Previous session did not end normally: {}", narutorise::incident::Summary(previous));
    REXLOG_INFO("[narutorise] Data root: {}", rex::path_to_utf8(active.data_root));
    REXLOG_INFO("[narutorise] Active config: {}", rex::path_to_utf8(active.config_path));
    for (const auto& message : prepared_data_->diagnostics)
      REXLOG_INFO("[narutorise] {}", message);
    if (game_data_root().empty()) return;
    LogGameData(game_data_root());
  }
  void LogGameData(const std::filesystem::path& root) {
    using narutorise::game_image::Status;
    game_identity_ = narutorise::game_image::Identify(root);
    const auto line = fmt::format("[narutorise] Game data: {} ({}); Title ID {:08X}, default.xex {}: {}",
                                  rex::path_to_utf8(root), game_identity_.disc_image ? "disc image" : "folder",
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
    if (!command_line_root_ && !prepared_data_->paths.data_root.empty()) {
      launch_marker_ = prepared_data_->paths.data_root / "launch.marker";
      std::error_code ec;
      start_failed_ = std::filesystem::exists(launch_marker_, ec);
      std::filesystem::create_directories(prepared_data_->paths.data_root, ec);
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

    const auto& toml_path = prepared_data_->paths.config_path;

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
                        rex::path_to_utf8(paths.update_data_root));
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
                    rex::path_to_utf8(paths.update_data_root));
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
                    rex::path_to_utf8(paths.update_data_root));
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
    narutorise::incident::Event("runtime-and-xex", "end");
    narutorise::reporting::FinishStartup();
    narutorise::translation::Initialize(prepared_data_->paths.mods_dir);
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
    narutorise::reporting::Close();
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

  void OnClosing(rex::ui::UIEvent& event) override {
    // The SDK uses _Exit here, so destructors and OnShutdown cannot flush it.
    narutorise::reporting::Close();
    narutorise::incident::ExpectedExit("window-close");
    rex::ReXApp::OnClosing(event);
  }

 private:
  bool FailDataBootstrap(const std::string& error) {
    std::string message = "Could not prepare the data folder.\n" + error;
    // The selected root may be locked, read-only or not resolved at all.
    // Use the SDK logger in temporary storage, never a second root's saves.
    try {
      bootstrap_log_file_ = REXCVAR_GET(log_file);
      auto config = rex::BuildLogConfig(bootstrap_log_file_.empty() ? nullptr : bootstrap_log_file_.c_str(),
                                        REXCVAR_GET(log_level), {});
#ifdef _WIN32
      const auto process = GetCurrentProcessId();
#else
      const auto process = getpid();
#endif
      const auto directory = std::filesystem::temp_directory_path() / "narutorise/bootstrap";
      config.app_name = "narutorise_bootstrap_" + std::to_string(process);
      config.log_dir = rex::path_to_utf8(directory);
      rex::InitLogging(config);
      REXLOG_ERROR("[narutorise] {}", message);
      rex::FlushLogging();
      message += "\nDiagnostic log: " + (bootstrap_log_file_.empty() ? config.log_dir : bootstrap_log_file_);
    } catch (const std::exception& e) {
      message += "\nCould not open diagnostic log: " + std::string(e.what());
    }
    message += "\nResolve the reported issue and retry. Use --data_root or REX_DATA_ROOT to select an isolated writable folder.";
    std::fprintf(stderr, "%s\n", message.c_str());
    std::fflush(stderr);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, message);
    return false;
  }

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
    std::string error;
    const bool copied = launcher_content::MirrorDlcTree(source, destination, error);
    if (!copied) REXLOG_WARN("[narutorise] DLC mirror preserved conflicting content: {}", error);
    return copied;
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
      const auto root = DlcSource();
      if (!std::filesystem::is_directory(root, ec)) {
        REXLOG_INFO("[narutorise] DLC folder not found ({}); running without DLC",
                    dlc_path_cvar);
      } else {
        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
          const auto filename = rex::path_to_utf8(entry.path().filename());

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
          auto rel = rex::path_to_utf8(std::filesystem::relative(pkg, root, rel_ec));
          if (rel.empty()) rel = rex::path_to_utf8(pkg.filename());

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
                  rex::path_to_utf8(shareable));
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
    menu_settings_.Initialize(rex::filesystem::GetExecutableFolder(), NativeRenderer(), prepared_data_->paths.config_path);
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
    if (const auto previous = narutorise::incident::Latest(true, true); !previous.empty()) {
      ui_.Notify(narutorise::menu::Text("diagnostics-recovered"), narutorise::menu::Text("diagnostics-open"),
                 [previous] { launcher_ui::OpenFolder(previous); });
      narutorise::incident::Acknowledge(previous);
    }
    if (NativeRenderer()) {
      ui_.SetTick([this] { ServiceMenu(); });
      menu_settings_.SetUtilities([this] { CreateDesktopShortcut(); }, [this] { CheckUpdates(); });
      if (REXCVAR_GET(check_for_updates))
        updates_ = std::make_unique<launcher_updates::Checker>(narutorise::Version(), prepared_data_->paths.data_root / "menu-updates.json");
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
    setup_operation_ = narutorise::incident::BeginOperation("setup-screen", narutorise::menu::SetupReasonName(reason));
    REXLOG_INFO("[narutorise] Setup presentation ready");
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
    if (setup_operation_)
      narutorise::incident::Event("setup-screen", "end", {}, std::exchange(setup_operation_, uint64_t{0}));
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
    menu_settings_.SetDataFolder(prepared_data_->paths, pending_data_root_, [this](narutorise::data::Mode mode) {
      app_context().CallInUIThreadDeferred([this, mode] { RequestDataLocation(mode); });
    });
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
      } else if (key == "crash_reporting") {
        const bool consent = menu_settings_.config().crash_reporting;
        rex::cvar::SetFlagByName(key, consent ? "true" : "false");
        if (!narutorise::reporting::SetConsent(consent) && consent) OfferSettingsRestart();
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
  void RequestDataLocation(narutorise::data::Mode mode) {
    if (closing_.load() || prepared_data_->paths.session_override) return;
    std::filesystem::path target;
    if (mode == narutorise::data::Mode::kCustom) {
      const auto choice = narutorise::menu::ChooseFile(window()->GetNativeWindowHandle(), true);
      if (!choice.error.empty()) {
        REXLOG_WARN("[narutorise] Data folder picker: {}", choice.error);
        ShowDataLocationError(); return;
      }
      if (choice.path.empty() || closing_.load()) return;
      target = choice.path;
    }
    const auto result = narutorise::data::RequestLocation(*prepared_data_, *data_environment_, mode, target, data_cli_);
    if (!result) {
      REXLOG_WARN("[narutorise] Data location selection: {}", result.error);
      ShowDataLocationError(); return;
    }
    if (result.value->pending_source || result.value->mode != prepared_data_->paths.mode)
      pending_data_root_ = result.value->data_root;
    else pending_data_root_.reset();
    menu_settings_.SetPendingDataLocation(pending_data_root_);
    if (pending_data_root_) OfferSettingsRestart();
  }
  void ShowDataLocationError() {
    using narutorise::menu::Text;
    ui_.ShowDialog(Text("storage-error"), Text("storage-error-help"), {{Text("cancel"), {}}});
  }
  std::filesystem::path DlcSource() const {
    const auto source = rex::cvar::GetFlagSource("dlc_source_path");
    const auto& base = source == rex::cvar::Source::kConfig
        ? prepared_data_->config_base : prepared_data_->paths.install_dir;
    return narutorise::data::ResolveDlcSource(prepared_data_->paths,
        REXCVAR_GET(dlc_source_path), base);
  }
  void OpenDlc() {
    menu_dlc_.Open(ui_, DlcSource(), user_data_root(), window()->GetNativeWindowHandle(),
        [this](std::function<void()> action) { app_context().CallInUIThreadDeferred(std::move(action)); }, [this] {
          if (setup_resume_) ui_.RefreshSetup({setup_reason_, game_identity_, narutorise::Version(), setup_paths_.game_data_root});
        });
  }
  void OfferSettingsRestart() {
    if (closing_.load()) return;
    using narutorise::menu::Text;
    std::string message = Text(pending_data_root_ ? "storage-pending-help" : "changed");
    if (game_running_) message += std::string("\n\n") + Text("exit-confirm");
    ui_.ShowDialog(Text("restart-required"), message, {
        {Text("restart-now"), [this] {
          app_context().CallInUIThreadDeferred([this] { RestartForSettings(); });
        }},
        {Text("later"), {}}});
  }
  std::vector<std::string> SessionExtraArguments() const {
    std::vector<std::string> arguments;
    if (restart_game_root_) arguments.push_back("--game_data_root=" + rex::path_to_utf8(*restart_game_root_));
    const auto source = rex::cvar::GetFlagSource("log_file");
    if (source == rex::cvar::Source::kCommandLine || source == rex::cvar::Source::kEnvironment) {
      const auto value = rex::to_path(rex::cvar::GetFlagByName("log_file"));
      if (!value.empty()) arguments.push_back("--log_file=" + rex::path_to_utf8(
          (value.is_absolute() ? value : prepared_data_->paths.initial_cwd / value).lexically_normal()));
    }
    return arguments;
  }
  void RestartForSettings() {
    if (closing_.load()) return;
    auto arguments = narutorise::launch::StorageArguments(prepared_data_->paths);
    const auto extra = SessionExtraArguments();
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    arguments.push_back("--setup");
    bool started = false;
#ifdef _WIN32
    const auto executable = prepared_data_->paths.install_dir / L"narutorise.exe";
    arguments.push_back("--restart_wait_pid=" + std::to_string(GetCurrentProcessId()));
    auto command = narutorise::launch::QuoteArgument(executable.wstring()) + L" " +
        narutorise::launch::CommandLine(arguments);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    started = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
        prepared_data_->paths.initial_cwd.c_str(), &startup, &process) != 0;
    if (started) {
      REXLOG_INFO("[narutorise] Restarting for saved settings: process {}", process.dwProcessId);
      CloseHandle(process.hThread); CloseHandle(process.hProcess);
    }
#else
    // Restart the external AppImage, never its temporary mount. The child waits
    // on EOF until the old process exits and the OS releases its data lease.
    const auto image = std::getenv("APPIMAGE");
    auto executable = image && *image ? rex::to_path(image) : prepared_data_->paths.install_dir / "narutorise";
    if (executable.is_relative()) executable = prepared_data_->paths.initial_cwd / executable;
    std::vector<std::string> values{rex::path_to_utf8(executable)};
    values.insert(values.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& value : values) argv.push_back(value.data());
    argv.push_back(nullptr);
    int wait_pipe[2];
    if (access(executable.c_str(), X_OK) == 0 && pipe2(wait_pipe, O_CLOEXEC) == 0) {
      const auto child = fork();
      if (child == 0) {
        close(wait_pipe[1]);
        char byte;
        while (read(wait_pipe[0], &byte, 1) < 0 && errno == EINTR) {}
        close(wait_pipe[0]);
        execv(executable.c_str(), argv.data());
        _exit(127);
      }
      close(wait_pipe[0]);
      if (child > 0) { restart_wait_fd_ = wait_pipe[1]; started = true; }
      else close(wait_pipe[1]);
    }
#endif
    if (started) { RemoveLaunchMarker(); RequestExit(); }
    else ui_.ShowDialog(narutorise::menu::Text("restart"), narutorise::menu::Text("save-failed"),
                        {{narutorise::menu::Text("cancel"), {}}});
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
        prepared_data_->paths.install_dir / "narutorise.exe", prepared_data_->paths, SessionExtraArguments());
    ui_.Notify(narutorise::menu::Text(ok ? "shortcut-done" : "save-failed"));
  }
  void CheckUpdates() {
    if (!updates_) updates_ = std::make_unique<launcher_updates::Checker>(narutorise::Version(), prepared_data_->paths.data_root / "menu-updates.json");
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
        if (!narutorise::menu::SaveGameRoot(prepared_data_->paths.config_path, result->root, error)) {
          ui_.ShowDialog(narutorise::menu::Text("save-failed"), error, {{narutorise::menu::Text("cancel"), {}}});
          return;
        }
        restart_game_root_ = result->root;
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
        REXLOG_INFO("[narutorise] Game selected: {}", rex::path_to_utf8(result->root));
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
    const auto incident_operation = narutorise::incident::BeginOperation("first-game-present");
    gpu::ui::UiLayer().SetFirstPresentCallback([this, incident_operation] { // Engine owner, after a successful game Present.
      narutorise::incident::Event("first-game-present", "end", {}, incident_operation);
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

  // Declared first so it outlives menu writers. ReXApp::OnDestroy tears down
  // the guest/runtime before member destruction; do not release in OnShutdown.
  std::optional<narutorise::data::PreparedData> prepared_data_;
  std::optional<narutorise::data::Environment> data_environment_;
  narutorise::data::Overrides data_cli_;
  std::optional<std::filesystem::path> pending_data_root_;
  std::string bootstrap_log_file_;
  std::optional<std::filesystem::path> restart_game_root_;
#ifndef _WIN32
  int restart_wait_fd_ = -1;  // Deliberately held until process exit (lease teardown).
#endif
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
  // The setup screen as an incident operation: the supervisor reads no stall
  // in the time the runtime waits for the player there.
  uint64_t setup_operation_ = 0;
#ifdef NARUTORISE_NATIVE_RENDERER_BOOTSTRAP
  gpu::ui::Screen setup_screen_{gpu::ui::UiLayer()};
#endif
};
