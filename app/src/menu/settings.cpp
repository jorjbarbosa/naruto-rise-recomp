// Saves each edit before changing the active state; unknown TOML keys survive.
#include "menu/settings.h"
#include "menu/ui.h"
#include "menu/text.h"
#include "diagnostics/incident.h"
#include "controls/runtime.h"
#include "wine.h"
#include "../menu/game_languages.h"
#include "../menu/graphics_devices.h"
#include "menu/folders.h"
#include "../menu/key_names.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <toml++/toml.hpp>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#ifdef _WIN32
#include <windows.h>
#endif
namespace narutorise::menu {
namespace {
std::string Utf8(const std::filesystem::path& path) { const auto s = path.u8string(); return {s.begin(), s.end()}; }
std::string Bool(bool value) { return Text(value ? "on" : "off"); }
}
void Settings::Initialize(std::filesystem::path directory, bool native_controls,
                          std::filesystem::path config_path) {
  edited_flags_.clear();
  controls_enabled_ = native_controls;
  directory_ = std::move(directory);
  config_path_ = config_path.empty() ? directory_ / "narutorise.toml" : std::move(config_path);
  Reload(); initial_ = config_;
  const auto effective_backend=rex::cvar::GetFlagByName("input_backend");
  controls::SetBackend(effective_backend.empty()?config_.input_backend:effective_backend);
  if(!controls_enabled_){controls::Publish({});initial_menu_options_=menu_options_;return;}
  controls::Hardware().Poll(controls::Backend(), true);
  if (!config_.controls.version && !load_failed_) {
    auto next = config_; next.controls = controls::Migrate(next.controls);
    if (Save(next)) config_ = std::move(next);
  }
  controls::Publish(config_.controls);
  initial_menu_options_ = menu_options_;
}
void Settings::SetPendingDataLocation(std::optional<std::filesystem::path> pending) {
  pending_data_root_ = std::move(pending);
  if (ui_ && panel_revision_ == ui_->ViewRevision())
    Redraw(pending_data_root_ ? "data-pending" : "data-active");
}
void Settings::Reload() {
  const auto& path=config_path_;
  const bool loaded=ConfigManager::Load(path, config_);
  load_failed_=std::filesystem::exists(path)&&!loaded;
  if (controls_enabled_ && config_.controls.version) controls::Publish(config_.controls);
  Localization::SetLanguageCode(config_.launcher_language);
  try {
    std::ifstream input(config_path_, std::ios::binary);
    const auto table = toml::parse(input);
    menu_options_.key = table["bind_game_menu"].value_or(std::string("F1"));
    menu_options_.chord = table["game_menu_chord"].value_or(std::string("Back+Start"));
    menu_options_.shaders = table["show_shader_indicator"].value_or(true);
    if (rex::ui::ParseVirtualKey(menu_options_.key) == rex::ui::VirtualKey::kNone) menu_options_.key = "F1";
  } catch (...) {}
}
bool Settings::Save(const GameConfig& config) {
  const auto& path = config_path_;
  auto temporary = path; temporary += ".settings.tmp";
  try {
    if(load_failed_)throw std::runtime_error("Refusing to replace a configuration that could not be loaded");
    if (std::filesystem::exists(path)) {
      std::ifstream input(path, std::ios::binary);
      (void)toml::parse(input); // Never overwrite a malformed configuration.
      std::filesystem::copy_file(path, temporary, std::filesystem::copy_options::overwrite_existing);
    }
    if (!ConfigManager::Save(temporary, config)) throw std::runtime_error("Configuration write failed");
    toml::table table;
    { std::ifstream input(temporary, std::ios::binary); table = toml::parse(input); }
    table.insert_or_assign("bind_game_menu", menu_options_.key);
    table.insert_or_assign("game_menu_chord", menu_options_.chord);
    table.insert_or_assign("show_shader_indicator", menu_options_.shaders);
    { std::ofstream output(temporary, std::ios::binary | std::ios::trunc); output << table; output.close();
      if (!output) throw std::runtime_error("Menu options write failed"); }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      throw std::runtime_error("Configuration replace failed");
#else
    std::filesystem::rename(temporary, path);
#endif
    return true;
  } catch (const std::exception& e) {
    REXLOG_ERROR("[narutorise] Settings save failed: {}", e.what());
    std::error_code ignored; std::filesystem::remove(temporary, ignored);
    if (ui_) ui_->ShowDialog(Text("save-failed"), Text("save-failed"), {{Text("cancel"), {}}});
    return false;
  }
}
void Settings::Open(Ui& ui, int tab, bool in_game, std::function<void()> choose_game,
                     std::function<void(const std::string&)> changed, std::function<void()> closed) {
  ui.HideDialog();
  if (!pending_config_) Reload();
  ui_ = &ui; group_ = std::clamp(tab, 0, 4); in_game_ = in_game;
  choose_game_ = std::move(choose_game); changed_ = std::move(changed); closed_ = std::move(closed);
  Redraw();
}
void Settings::Change(const std::string& key, std::function<void(GameConfig&)> change) {
  if (pending_config_) return;
  auto next = config_; change(next);
  if (!Save(next)) return;
  if (in_game_ && request_live_) {
    const auto ticket = request_live_(key, next);
    if (ticket) {
      pending_config_ = std::move(next); pending_key_ = key; pending_ticket_ = ticket;
      Redraw(key); return;
    }
  }
  CommitChange(key, std::move(next));
}
bool Settings::AnswerCrashReporting(bool consent) {
  auto next = config_;
  next.crash_reporting = consent;
  next.crash_reporting_asked = true;
  if (pending_config_ || !Save(next)) return false;
  config_ = std::move(next);
  // Applied in this session: nothing for the settings to mark as changed.
  initial_.crash_reporting = consent;
  initial_.crash_reporting_asked = true;
  return true;
}
void Settings::CommitChange(const std::string& key, GameConfig next, std::string focus) {
  config_ = std::move(next);
  edited_flags_.insert(key);
  if (key == "user_language") {
    edited_flags_.insert("translation_mod");
  } else if (key == "resolution_scale") {
    edited_flags_.insert("draw_resolution_scale_x");
    edited_flags_.insert("draw_resolution_scale_y");
    edited_flags_.insert("internal_resolution_scale");
  } else if (key == "vsync") {
    edited_flags_.insert("d3d12_host_vsync");
  } else if (key == "native_backend") {
    edited_flags_.insert("graphics_device");
  }
  Localization::SetLanguageCode(config_.launcher_language);
  if (changed_) changed_(key);
  if (ui_ && panel_revision_ == ui_->ViewRevision()) Redraw(focus.empty() ? key : std::move(focus));
}
void Settings::PollLive() {
  if (!pending_config_ || !result_live_) return;
  const auto result = result_live_();
  if (result.revision < pending_ticket_) return;
  const auto key = std::move(pending_key_);
  auto next = std::move(*pending_config_); pending_config_.reset(); pending_ticket_ = 0;
  if (result.applied) {
    CommitChange(key, std::move(next), ui_ ? ui_->PanelFocus() : std::string());
    REXLOG_INFO("[narutorise] Live setting confirmed: {} (revision {})", key, result.revision);
  }
  else {
    const bool restored = Save(config_);
    if (ui_ && restored) {
      if (panel_revision_ == ui_->ViewRevision()) Redraw(key);
      ui_->ShowDialog(Text("settings"), Text("apply-settings-failed"), {{Text("cancel"), {}}});
    }
  }
}
void Settings::SaveMenuOptions(const std::string& key, MenuOptions next) {
  if (pending_config_) return;
  auto previous = menu_options_; menu_options_ = std::move(next);
  if (!Save(config_)) { menu_options_ = std::move(previous); return; }
  edited_flags_.insert(key);
  if (changed_) changed_(key);
  Redraw(key);
}
void Settings::Redraw(std::string focus) {
  ui_->ShowPanel(Text("settings"), {Tr(TextId::GroupGame), Tr(TextId::GroupDisplay), Tr(TextId::GroupQuality),
      Tr(TextId::GroupControls), Tr(TextId::GroupAdvanced)}, group_, Rows(group_),
      [this](int group) { group_ = group; Redraw(); },
      [this] { panel_revision_ = 0; ui_->HidePanel(); ui_->CancelKeyCapture(); if (closed_) closed_(); }, std::move(focus));
  panel_revision_ = ui_->ViewRevision();
}
std::vector<PanelRow> Settings::Rows(int group) {
  std::vector<PanelRow> rows;
  const auto choice = [&](std::string key, TextId label, TextId description, std::vector<std::string> values,
                           int selected, bool modified, bool restart, std::function<void(GameConfig&, int)> set) {
    selected = std::clamp(selected, 0, int(values.size()) - 1);
    rows.push_back({key, Tr(label), values[size_t(selected)], Tr(description), modified, restart, values, selected,
        [this, key, set](int value) { Change(key, [set, value](GameConfig& config) { set(config, value); }); }, {}});
  };
  const auto toggle = [&](const char* key, TextId label, TextId description, bool GameConfig::* member, bool restart = true) {
    choice(key, label, description, {Text("off"), Text("on")}, config_.*member ? 1 : 0,
        config_.*member != initial_.*member, restart, [member](GameConfig& config, int value) { config.*member = value != 0; });
  };
  if (group == 0) {
    rows.push_back({"custom_game_root", Tr(TextId::TabGameFiles), config_.custom_game_root, Text("game-hint"), false,
        in_game_, {}, 0, {}, choose_game_});
    const auto mods = launcher_ui::DiscoverTranslationMods(
        data_paths_ ? data_paths_->mods_dir : config_path_.parent_path() / "mods");
    const auto languages = launcher_ui::GameLanguages(config_, mods);
    std::vector<std::string> names;
    for (const auto& language : languages.options) names.push_back(language.label);
    choice("user_language", TextId::GameLanguageLabel, TextId::GameLanguageDescription, names, languages.selected,
        config_.user_language != initial_.user_language || config_.translation_mod != initial_.translation_mod, true,
        [languages](GameConfig& config, int index) { launcher_ui::SelectGameLanguage(config, languages.options.at(size_t(index))); });
    toggle("skip_intro_videos", TextId::SkipIntroLabel, TextId::SkipIntroDescription, &GameConfig::skip_intro_videos);
    names.clear(); int language = 0;
    for (const auto& item : Localization::GetLanguages()) {
      if (config_.launcher_language == item.code) language = int(names.size());
      names.emplace_back(item.name);
    }
    PanelRow menu_language{"launcher_language", Text("language"), names[size_t(language)], Text("language"),
        config_.launcher_language != initial_.launcher_language, false, names, language,
        [this](int value) { Change("launcher_language", [value](GameConfig& config) {
          config.launcher_language = Localization::GetLanguages().at(size_t(value)).code; }); }, {}};
    rows.push_back(std::move(menu_language));
  } else if (group == 1) {
    std::vector<std::string> apis, api_values;
#if !defined(NARUTORISE_NATIVE_D3D12_DISABLED)
    if (!narutorise::WineVersion()) { apis.push_back("Direct3D 12"); api_values.push_back("d3d12"); }
#endif
#if defined(NARUTORISE_NATIVE_VULKAN)
    apis.push_back("Vulkan"); api_values.push_back("vulkan");
#endif
    int api = 0;
    for (size_t i = 0; i < api_values.size(); ++i) if (api_values[i] == config_.native_backend) api = int(i);
    if (!apis.empty()) choice("native_backend", TextId::RendererLabel, TextId::RendererDescription, apis, api,
        config_.native_backend != initial_.native_backend, true,
        [api_values](GameConfig& config, int value) { config.native_backend = api_values.at(size_t(value)); config.graphics_device.clear(); });
    auto devices = GraphicsDevices(config_.native_backend == "vulkan");
    devices.insert(devices.begin(), Tr(TextId::GraphicsDeviceAuto));
    int device = 0;
    for (size_t i = 1; i < devices.size(); ++i) if (devices[i] == config_.graphics_device) device = int(i);
    if (!config_.graphics_device.empty() && !device) { device = int(devices.size()); devices.push_back(config_.graphics_device); }
    choice("graphics_device", TextId::GraphicsDeviceLabel, TextId::GraphicsDeviceDescription, devices, device,
        config_.graphics_device != initial_.graphics_device, true,
        [devices](GameConfig& config, int value) { config.graphics_device = value ? devices.at(size_t(value)) : ""; });
    constexpr double scales[] = {1, 1.5, 2, 3, 4};
    const double current = config_.internal_resolution_scale > 0 ? config_.internal_resolution_scale : config_.resolution_scale;
    int scale = 0; for (int i = 1; i < 5; ++i) if (std::abs(scales[i] - current) < std::abs(scales[scale] - current)) scale = i;
    choice("resolution_scale", TextId::ResolutionLabel, TextId::ResolutionDescription,
        {"1280×720 (1x)", "1920×1080 (1.5x)", "2560×1440 (2x)", "3840×2160 (3x)", "5120×2880 (4x)"}, scale,
        config_.resolution_scale != initial_.resolution_scale || config_.internal_resolution_scale != initial_.internal_resolution_scale,
        true, [](GameConfig& config, int value) {
          constexpr int integer[] = {1, 1, 2, 3, 4};
          config.internal_resolution_scale = value == 1 ? 1.5 : 0;
          config.resolution_scale = config.draw_resolution_scale_x = config.draw_resolution_scale_y = integer[value];
        });
    std::vector<std::string> levels; for (int i = 0; i <= 100; ++i) levels.push_back(std::to_string(i));
    choice("brightness", TextId::BrightnessLabel, TextId::BrightnessDescription, levels,
        int(std::lround(config_.brightness * 100)), config_.brightness != initial_.brightness, true,
        [](GameConfig& config, int value) { config.brightness = value / 100.0; });
    choice("frame_rate", TextId::FrameRateLabel, TextId::FrameRateDescription, {"30", "60", "120"},
        config_.frame_rate >= 120 ? 2 : config_.frame_rate >= 60 ? 1 : 0, config_.frame_rate != initial_.frame_rate, true,
        [](GameConfig& config, int value) { config.frame_rate = value == 2 ? 120 : value == 1 ? 60 : 30; });
    choice("vsync", TextId::VsyncLabel, TextId::VsyncNativeDescription, {Text("off"), Text("on")},
        config_.vsync ? 1 : 0, config_.vsync != initial_.vsync, true,
        [](GameConfig& config, int value) { config.vsync = config.d3d12_host_vsync = value != 0; });
    toggle("fullscreen", TextId::FullscreenLabel, TextId::FullscreenDescription, &GameConfig::fullscreen, false);
  } else if (group == 2) {
    choice("msaa_samples", TextId::MsaaLabel, TextId::MsaaDescription, {"2x", Tr(TextId::MsaaOriginal), "8x"},
        config_.msaa_samples == 2 ? 0 : config_.msaa_samples >= 8 ? 2 : 1, config_.msaa_samples != initial_.msaa_samples, true,
        [](GameConfig& config, int value) { config.msaa_samples = value == 0 ? 2 : value == 2 ? 8 : 0; });
    choice("swap_post_effect", TextId::FxaaLabel, TextId::FxaaDescription, {Tr(TextId::FxaaOff), Tr(TextId::FxaaNormal), Tr(TextId::FxaaExtreme)},
        config_.swap_post_effect == "fxaa_extreme" ? 2 : config_.swap_post_effect == "fxaa" ? 1 : 0,
        config_.swap_post_effect != initial_.swap_post_effect, true,
        [](GameConfig& config, int value) { config.swap_post_effect = value == 2 ? "fxaa_extreme" : value == 1 ? "fxaa" : "none"; });
    choice("present_effect", TextId::PostProcessingLabel, TextId::PostProcessingDescription,
        {Tr(TextId::PostBilinear), Tr(TextId::PostCas), Tr(TextId::PostFsr)},
        config_.present_effect.starts_with("fsr") ? 2 : config_.present_effect.starts_with("cas") ? 1 : 0,
        config_.present_effect != initial_.present_effect, true,
        [](GameConfig& config, int value) { config.present_effect = value == 2 ? "fsr" : value == 1 ? "cas" : "bilinear"; });
    choice("anisotropic_override", TextId::TextureFilteringLabel, TextId::TextureFilteringDescription,
        {Tr(TextId::AnisoDefault), "2x", "4x", "8x", "16x"}, config_.anisotropic_override - 1,
        config_.anisotropic_override != initial_.anisotropic_override, true,
        [](GameConfig& config, int value) { config.anisotropic_override = value + 1; });
    toggle("transparency_antialiasing", TextId::TransparencyAaLabel, TextId::TransparencyAaDescription, &GameConfig::transparency_antialiasing);
  } else if (group == 3) {
    rows.push_back({"configure-controls", Text("c-configure"), "›", Text("c-configure-help"), false, false, {}, 0, {}, [this] { Controls(); }});

    choice("input_backend", TextId::SdlInputLabel, TextId::SdlInputDescription, {"XInput", "SDL"},
        config_.input_backend == "xinput" ? 0 : 1, config_.input_backend != initial_.input_backend, true,
        [](GameConfig& config, int value) { config.input_backend = value ? "sdl" : "xinput"; });
    rows.push_back({"bind_game_menu", Text("menu-key"), menu_options_.key, Text("menu-key"),
        menu_options_.key != initial_menu_options_.key, false, {}, 0, {}, [this] {
      ui_->ShowDialog(Text("menu-key"), Tr(TextId::KeysHint), {{Text("cancel"), [this] { ui_->CancelKeyCapture(); }}});
      ui_->CaptureKey([this](std::string key) {
        ui_->HideDialog();
        if (key.empty()) return;
        if (key == "Escape" || key == "Return" || key == "Shift" || key == "Alt" || key == "Control" ||
            key == "F3" || key == "F4" || key == "F7" || key == "F11" || key == "Backtick") return;
        auto next = menu_options_; next.key = std::move(key); SaveMenuOptions("bind_game_menu", next);
      });
    }});
    const std::vector<std::string> chords = {"Back+Start", "LB+RB+Start", "L3+R3"};
    const std::vector<std::string> chord_names = {"Back + Start", "LB + RB + Start", "L3 + R3"};
    const auto found = std::find(chords.begin(), chords.end(), menu_options_.chord);
    const int selected = found == chords.end() ? 0 : int(found - chords.begin());
    rows.push_back({"game_menu_chord", Text("pad-shortcut"), chord_names[size_t(selected)], Text("shortcut-hint"),
        menu_options_.chord != initial_menu_options_.chord, false, chord_names, selected,
        [this, chords](int value) { auto next = menu_options_; next.chord = chords.at(size_t(value)); SaveMenuOptions("game_menu_chord", next); }, {}});
  } else {
    rows.push_back({"show_startup_menu", Text("startup-menu"), Bool(config_.show_startup_menu), Text("startup-menu-help"),
        config_.show_startup_menu != initial_.show_startup_menu, false,
        {Text("off"), Text("on")}, config_.show_startup_menu ? 1 : 0,
        [this](int value) { Change("show_startup_menu", [value](GameConfig& c) { c.show_startup_menu = value != 0; }); }, {}});
    if (updates_) rows.push_back({"updates", Tr(TextId::UpdatesCheck), "", Text("update-hint"), false, false, {}, 0, {}, updates_});
    if (shortcut_) rows.push_back({"shortcut", Text("shortcut"), "", Text("shortcut"), false, false, {}, 0, {}, shortcut_});
    if (game_files_) rows.push_back({"game_files", Text("game-files"), "", Text("game-files-help"), false, false, {}, 0, {}, game_files_});
    toggle("show_fps_overlay", TextId::FpsOverlayLabel, TextId::FpsOverlayDescription, &GameConfig::show_fps_overlay, false);
    rows.push_back({"show_shader_indicator", Text("shader-option"), Bool(menu_options_.shaders), Text("shader-option"),
        menu_options_.shaders != initial_menu_options_.shaders, false, {Text("off"), Text("on")}, menu_options_.shaders ? 1 : 0,
        [this](int value) { auto next = menu_options_; next.shaders = value != 0; SaveMenuOptions("show_shader_indicator", next); }, {}});
    choice("max_frame_latency", TextId::FrameLatencyLabel, TextId::FrameLatencyDescription,
        config_.native_backend == "vulkan" ? std::vector<std::string>{Tr(TextId::FrameLatencyLowest)} :
        std::vector<std::string>{Tr(TextId::FrameLatencyLowest), "2", "3"}, config_.max_frame_latency - 1,
        config_.max_frame_latency != initial_.max_frame_latency, true,
        [](GameConfig& config, int value) { config.max_frame_latency = value + 1; });
    toggle("triple_buffering", TextId::TripleBufferingLabel, TextId::TripleBufferingDescription, &GameConfig::triple_buffering);
    rows.push_back({"crash_reporting", Text("crash-reporting"), Bool(config_.crash_reporting), Text("crash-reporting-help"),
        config_.crash_reporting != initial_.crash_reporting, true, {Text("off"), Text("on")}, config_.crash_reporting ? 1 : 0,
        [this](int value) { Change("crash_reporting", [value](GameConfig& config) {
          config.crash_reporting = value != 0; config.crash_reporting_asked = true; }); }, {}});
    if (!incident::Directory().empty()) {
      rows.push_back({"diagnostics-open", Text("diagnostics-open"), "", Text("diagnostics-help"), false, false, {}, 0, {},
          [] { launcher_ui::OpenFolder(incident::ReportsDirectory()); }});
      rows.push_back({"diagnostics-snapshot", Text("diagnostics-snapshot"), "", Text("diagnostics-snapshot-help"), false, false, {}, 0, {},
          [this] { incident::RequestSnapshot(); ui_->Notify(Text("diagnostics-requested")); }});
      rows.push_back({"diagnostics-export", Text("diagnostics-export"), "", Text("diagnostics-export-help"), false, false, {}, 0, {},
          [this] {
            // The newest session, and the newest failed one when that is older:
            // a player often reopens the game a few times before exporting.
            const auto latest = incident::Latest();
            if (latest.empty()) { ui_->Notify(Text("diagnostics-none")); return; }
            std::vector<std::filesystem::path> sessions{latest};
            if (const auto failed = incident::Latest(true); !failed.empty() && failed != latest) sessions.push_back(failed);
            const auto exports = incident::ReportsDirectory().parent_path() / "incident-exports";
            for (const auto& session : sessions) {
              const auto destination = exports / (session.filename().string() + ".zip");
              std::string error;
              if (!std::filesystem::exists(destination) && !incident::Export(session, destination, error)) {
                ui_->Notify(Text("diagnostics-export-failed") + std::string(" ") + error); return;
              }
            }
            launcher_ui::OpenFolder(exports);
          }});
    }
    if (data_paths_) {
      const auto active = data_paths_->data_root;
      const bool blocked = data_paths_->session_override;
      PanelRow current{"data-active", Text("storage-active"), Utf8(active),
          std::string(Text("storage-help")) + "\n\n" + Utf8(active)};
      current.section = Text("storage-section"); rows.push_back(std::move(current));
      rows.push_back({"data-open", Text("storage-open"), "", Utf8(active), false, false, {}, 0, {},
          [active] { launcher_ui::OpenFolder(active); }});
      if (data_paths_->user_source != data::Source::kDefault) {
        const auto guest = data_paths_->user_data_root;
        rows.push_back({"data-guest", Text("storage-guest"), Utf8(guest),
            std::string(Text("storage-guest-help")) + "\n\n" + Utf8(guest), false, false, {}, 0, {},
            [guest] { launcher_ui::OpenFolder(guest); }});
      }
      const auto action = [&](const char* id, const char* label, data::Mode mode) {
        PanelRow row{id, Text(label), "", Text(blocked ? "storage-override" : "storage-help"), false, true};
        if (!blocked && select_data_location_) row.activate = [fn = select_data_location_, mode] { fn(mode); };
        rows.push_back(std::move(row));
      };
      action("data-choose", "storage-choose", data::Mode::kCustom);
      action("data-portable", "storage-portable", data::Mode::kPortable);
      action("data-default", "storage-default", data::Mode::kDefault);
      if (pending_data_root_)
        rows.push_back({"data-pending", Text("storage-pending"), Utf8(*pending_data_root_),
            std::string(Text("storage-pending-help")) + "\n\n" + Utf8(*pending_data_root_), true, true});
    }
  }
  for (auto& row : rows) {
    if (controls_enabled_ && request_live_ && (row.id == "resolution_scale" || row.id == "brightness" ||
        row.id == "frame_rate" || row.id == "vsync" || row.id == "msaa_samples" || row.id == "swap_post_effect" ||
        row.id == "present_effect" || row.id == "anisotropic_override" || row.id == "transparency_antialiasing" ||
        row.id == "max_frame_latency" || row.id == "triple_buffering" || row.id == "skip_intro_videos")) row.restart = false;
    if (pending_config_ && row.id == pending_key_) { row.value += " …"; row.description = Text("applying-settings"); }
    row.inline_adjust = row.id == "brightness" || row.id == "mnk_sensitivity";
    if (row.id == "controller") {
      row.description += "\n\n" + row.value;
      if (row.value == Tr(TextId::NoControllerDetected)) row.value = Text("not-connected");
    }
    if (row.id == "native_backend") row.label = Text("graphics-api");
    if (row.id == "input_backend") row.label = Text("input-api");
    if (row.id == "mnk_mode") row.label = Text("keyboard-mouse");
    if (row.id == "mnk_mouse") row.label = Text("mouse-camera");
    if (row.id == "mnk_sensitivity") row.label = Text("sensitivity");
    if (row.id == "keys") row.label = Text("key-mapping");
    if (row.id == "skip_intro_videos") row.label = Text("skip-videos");
    if (group == 1 && row.id == "native_backend") row.section = Text("video");
    if (group == 1 && row.id == "brightness") row.section = Text("display-section");
    if (group == 3 && row.id == "input_backend") row.section = Text("input-section");
    if (group == 3 && row.id == "mnk_mode") row.section = Text("keyboard-mouse");
    if (group == 3 && row.id == "bind_game_menu") row.section = Text("menu-section");
    if (group == 4 && row.id == "show_startup_menu") row.section = Text("general-section");
    if (group == 4 && row.id == "show_fps_overlay") row.section = Text("overlay-section");
    if (group == 4 && row.id == "max_frame_latency") row.section = Text("performance-section");
    if (group == 4 && row.id == "crash_reporting") row.section = Text("diagnostics-section");
  }
  return rows;
}
void Settings::Controls() {
  if (pending_config_) return;
  if(!config_.controls.version){ui_->ShowDialog(Text("save-failed"),Text("save-failed"),{{Text("cancel"),{}}});return;}
  ui_->ShowControls(config_.controls, [this](const controls::Config& next) {
    auto proposed = config_; proposed.controls = next;
    if (!Save(proposed)) return false;
    config_ = std::move(proposed); controls::Publish(config_.controls);
    if (changed_) changed_("controls");
    return true;
  }, [this] { Redraw("configure-controls"); }, menu_options_.key);
}
bool Settings::ApplyBeforeGame() const {
  try {
    std::ifstream input(config_path_, std::ios::binary);
    const auto table = toml::parse(input);
    // The SDK already loaded the initial configuration with CLI/environment
    // precedence. Only explicit menu edits may override those session values.
    for (const auto& name : edited_flags_) {
      if (!rex::cvar::GetFlagInfo(name)) continue;
      const auto node = table[name];
      std::string value;
      if (node.is_string()) value = node.value<std::string>().value();
      else if (node.is_boolean()) value = node.value<bool>().value() ? "true" : "false";
      else if (node.is_integer()) value = std::to_string(node.value<int64_t>().value());
      else if (node.is_floating_point()) value = std::to_string(node.value<double>().value());
      else continue;
      if (!rex::cvar::SetFlagByName(name, value)) {
        REXLOG_ERROR("[narutorise] Edited setting could not be applied: {}", name);
        return false;
      }
    }
    controls::SetBackend(rex::cvar::GetFlagByName("input_backend"));
    controls::Publish(controls_enabled_?config_.controls:controls::Config{});
    return true;
  } catch (const std::exception& e) { REXLOG_WARN("[narutorise] Settings could not be reloaded: {}", e.what()); }
  return false;
}
}  // namespace narutorise::menu
