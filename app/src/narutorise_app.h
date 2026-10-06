// narutorise - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/rex_app.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xcontent.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/window.h>

#include "ultrawide.h"

REXCVAR_DEFINE_BOOL(show_fps_overlay, false, "narutorise/UI",
                    "Show FPS and frametime overlay (toggle with F1)");

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

class FpsOverlayDialog : public rex::ui::ImGuiDialog {
 public:
  using GuestStatsProvider = std::function<rex::ui::FrameStats()>;

  explicit FpsOverlayDialog(rex::ui::ImGuiDrawer* drawer,
                            GuestStatsProvider guest_stats = {})
      : rex::ui::ImGuiDialog(drawer), guest_stats_(std::move(guest_stats)) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    // The host rate counts window presents (io.Framerate), which can run far
    // above the game's own frame rate when presentation is independent
    // (high-refresh display, tearing allowed). Game speed follows the guest
    // rate, so the overlay leads with it and keeps the host rate as context.
    rex::ui::FrameStats guest{};
    if (guest_stats_) {
      guest = guest_stats_();
    }
    const bool have_guest = guest.frame_count > 0;

    const double host_fps = io.Framerate;
    const double fps = have_guest ? guest.fps : host_fps;
    const double ft_ms =
        have_guest ? guest.frame_time_ms : io.DeltaTime * 1000.0;

    if (smoothed_fps_ == 0.0) {
      smoothed_fps_ = fps;
      smoothed_ft_ = ft_ms;
      smoothed_host_fps_ = host_fps;
    } else {
      smoothed_fps_ = smoothed_fps_ * 0.85 + fps * 0.15;
      smoothed_ft_ = smoothed_ft_ * 0.85 + ft_ms * 0.15;
      smoothed_host_fps_ = smoothed_host_fps_ * 0.85 + host_fps * 0.15;
    }

    frame_history_[history_idx_] = static_cast<float>(smoothed_ft_);
    history_idx_ = (history_idx_ + 1) % kHistorySize;

    ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.65f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);

    bool visible = true;
    if (ImGui::Begin("##fps_overlay", &visible,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoFocusOnAppearing)) {
      ImGui::SetWindowFontScale(2.0f);
      ImU32 fps_color = smoothed_fps_ >= 55.0f ? IM_COL32(80, 255, 80, 255) :
                       smoothed_fps_ >= 30.0f ? IM_COL32(255, 220, 60, 255) :
                                                IM_COL32(255, 80, 80, 255);
      ImGui::PushStyleColor(ImGuiCol_Text, fps_color);
      ImGui::Text("%s: %.0f FPS", have_guest ? "Guest" : "Host", smoothed_fps_);
      ImGui::PopStyleColor();
      ImGui::SetWindowFontScale(1.0f);

      ImGui::SetWindowFontScale(1.3f);
      ImGui::Text("%.1f ms", smoothed_ft_);
      ImGui::SetWindowFontScale(1.0f);

      if (have_guest) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(160, 160, 160, 255));
        ImGui::Text("Host: %.0f FPS", smoothed_host_fps_);
        ImGui::PopStyleColor();
      }

      ImGui::Spacing();
      ImGui::PlotLines("##frametime", frame_history_.data(),
                       static_cast<int>(kHistorySize),
                       static_cast<int>(history_idx_), nullptr,
                       0.0f, 50.0f, ImVec2(220, 50));
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
  }

 private:
  static constexpr size_t kHistorySize = 120;
  std::array<float, kHistorySize> frame_history_{};
  size_t history_idx_ = 0;
  double smoothed_fps_ = 0.0;
  double smoothed_ft_ = 0.0;
  double smoothed_host_fps_ = 0.0;
  GuestStatsProvider guest_stats_;
};

class NarutoriseApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<NarutoriseApp>(new NarutoriseApp(ctx, "narutorise",
        PPCImageConfig));
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (paths.game_data_root.empty() ||
        !std::filesystem::is_directory(paths.game_data_root)) {
      const auto exe_dir = rex::filesystem::GetExecutableFolder();

      // Verifica se há custom_game_root definido no narutorise.toml
      const std::filesystem::path toml_path = exe_dir / "narutorise.toml";
      std::optional<toml::table> toml_tbl;
      if (std::filesystem::exists(toml_path)) {
        try {
          toml_tbl = toml::parse_file(toml_path.string());
        } catch (...) {
        }
      }

      if (toml_tbl) {
        if (auto v = (*toml_tbl)["custom_game_root"].value<std::string>()) {
          std::error_code ec;
          if (!v->empty() && std::filesystem::is_directory(*v, ec)) {
            paths.game_data_root = std::filesystem::canonical(*v, ec);
            REXLOG_INFO("[narutorise] Using custom_game_root from TOML: {}",
                        paths.game_data_root.string());
          }
        }
      }

      if (paths.game_data_root.empty()) {
        const std::filesystem::path candidates[] = {
            exe_dir / "game_root",
            exe_dir / "game",
            exe_dir / "../../../../game_root",
            exe_dir / "../../../game_root",
        };
        for (const auto& candidate : candidates) {
          std::error_code ec;
          if (std::filesystem::is_directory(candidate, ec)) {
            paths.game_data_root = std::filesystem::canonical(candidate, ec);
            REXLOG_INFO("[narutorise] Auto-detected game_data_root: {}",
                        paths.game_data_root.string());
            break;
          }
        }
      }
    }

    ConfigureUpdateDataRoot(paths);
  }

  void ConfigureUpdateDataRoot(rex::PathConfig& paths) {
    if (paths.game_data_root.empty()) return;

    const auto exe_dir = rex::filesystem::GetExecutableFolder();
    const std::filesystem::path toml_path = exe_dir / "narutorise.toml";

    // 1. Explicit TOML setting.
    if (std::filesystem::exists(toml_path)) {
      try {
        auto tbl = toml::parse_file(toml_path.string());
        if (auto v = tbl["update_data_root"].value<std::string>()) {
          std::error_code ec;
          if (!v->empty() && std::filesystem::is_directory(*v, ec)) {
            paths.update_data_root = std::filesystem::canonical(*v, ec);
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
    ApplyUltrawidePatch(runtime()->virtual_membase());
  }

  void OnPostSetup() override {
    SeedShaderStorage();
    AutoInstallDlc();

    // Guest frame rate (measured per guest swap by the command processor) for
    // the SDK debug overlay (F3).
    SetGuestFrameStats(&ReadGuestFrameStats);

    // Encerramento limpo via Alt+F4
    rex::ui::RegisterBind("bind_exit_game", "Alt+F4", "Exit Game", [this]() {
      if (window()) {
        window()->RequestClose();
      }
    });

    // Overlay de FPS (F1 para alternar)
    if (REXCVAR_GET(show_fps_overlay) && imgui_drawer()) {
      fps_overlay_ =
          std::make_unique<FpsOverlayDialog>(imgui_drawer(), &ReadGuestFrameStats);
    }
    rex::ui::RegisterBind("bind_fps_overlay", "F1",
                          "Toggle FPS overlay", [this]() {
      if (fps_overlay_) {
        fps_overlay_.reset();
        rex::cvar::SetFlagByName("show_fps_overlay", "false");
      } else if (imgui_drawer()) {
        fps_overlay_ =
            std::make_unique<FpsOverlayDialog>(imgui_drawer(), &ReadGuestFrameStats);
        rex::cvar::SetFlagByName("show_fps_overlay", "true");
      }
    });

    // Callback para reaplicar o aspect ratio em tempo real caso a cvar mude
    rex::cvar::RegisterChangeCallback(
        "ultrawide_target_aspect", [this](std::string_view, std::string_view) {
          if (runtime()) {
            ApplyUltrawidePatch(runtime()->virtual_membase());
          }
        });
  }

  void OnShutdown() override {
    rex::cvar::UnregisterChangeCallbacks("ultrawide_target_aspect");
    rex::ui::UnregisterBind("bind_exit_game");
    rex::ui::UnregisterBind("bind_fps_overlay");
    fps_overlay_.reset();
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

  std::unique_ptr<FpsOverlayDialog> fps_overlay_;
};
