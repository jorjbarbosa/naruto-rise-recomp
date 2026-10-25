#include "config_manager.h"

#include <fstream>
#include <algorithm>
#include <iostream>
#include <toml++/toml.hpp>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

bool ConfigManager::Load(const std::filesystem::path& toml_path, GameConfig& config) {
  if (!std::filesystem::exists(toml_path)) {
    return false;
  }
  GameConfig loaded = config;
  try {
    // Open the native filesystem path (UTF-16 on Windows), not an ANSI path
    // passed to a parser expecting UTF-8. Otherwise accented install folders
    // silently fall back to defaults and lose user settings on the next save.
    std::ifstream input(toml_path, std::ios::binary);
    if (!input) return false;
    auto tbl = toml::parse(input);
    if (auto v = tbl["gpu_plugin"].value<std::string>()) loaded.gpu_plugin = *v;
    if (auto v = tbl["renderer"].value<std::string>()) loaded.renderer = *v == "xenos" ? "xenos" : "native";
    // The launcher now selects the native graphics API directly. Migrate old
    // Xenos selections while preserving the saved native backend.
    if (kNativeRendererBuilt) loaded.renderer = "native";
    if (auto v = tbl["graphics_device"].value<std::string>()) loaded.graphics_device = *v;
    if (auto v = tbl["native_backend"].value<std::string>()) loaded.native_backend = *v;
    if (auto v = tbl["d3d12_readback_resolve"].value<bool>()) loaded.d3d12_readback_resolve = *v;
    if (auto v = tbl["skip_intro_videos"].value<bool>()) loaded.skip_intro_videos = *v;
    if (auto v = tbl["user_language"].value<std::uint32_t>()) loaded.user_language = *v;
    if (auto v = tbl["translation_mod"].value<std::string>()) loaded.translation_mod = *v;
    if (auto v = tbl["log_verbose"].value<bool>()) loaded.log_verbose = *v;
    if (auto v = tbl["log_level"].value<std::string>()) loaded.log_level = *v;
    if (auto v = tbl["protect_zero"].value<bool>()) loaded.protect_zero = *v;
    if (auto v = tbl["input_backend"].value<std::string>()) loaded.input_backend = *v;
    if (auto v = tbl["mnk_mode"].value<bool>()) loaded.mnk_mode = *v;
    if (auto v = tbl["mnk_mouse"].value<bool>()) loaded.mnk_mouse = *v;
    if (auto v = tbl["mnk_sensitivity"].value<double>()) loaded.mnk_sensitivity = *v;
    // A configuration from before the key options keeps the default layout.
    for (std::size_t i = 0; i < kKeyActionCount; ++i) {
      if (auto v = tbl[KeyActionOption(i)].value<std::string>()) loaded.key_binds[i] = *v;
    }
    using namespace narutorise::controls;
    loaded.controls = {};
    loaded.controls.ports[0].keys = loaded.key_binds;
    loaded.controls.ports[0].mouse = loaded.mnk_mouse;
    loaded.controls.ports[0].sensitivity = std::clamp(loaded.mnk_sensitivity, .01, 10.0);
    if (loaded.mnk_mouse) {
      constexpr const char* motion[] = {"MouseUp","MouseDown","MouseLeft","MouseRight"};
      for (int i = 0; i < 4; ++i) loaded.controls.ports[0].keys[16+i] += std::string(",") + motion[i];
    }
    if (auto* controls = tbl["controls"].as_table()) {
      loaded.controls.version = (*controls)["version"].value_or(1);
      for (size_t i = 0; i < kPorts; ++i) if (auto* port = (*controls)["player" + std::to_string(i+1)].as_table()) {
        auto& p = loaded.controls.ports[i];
        p.device = (*port)["device"].value_or(std::string{});
        p.device_name = (*port)["device_name"].value_or(std::string{});
        p.mouse = (*port)["mouse"].value_or(true);
        p.deadzones = (*port)["deadzones"].value_or(true);
        p.sensitivity = (*port)["sensitivity"].value_or(1.0);
        for (size_t a = 0; a < kKeyActionCount; ++a) {
          if (auto s = (*port)["pad"][KeyActionOption(a)].value<std::string>()) p.pad[a] = Decode(*s);
          if (auto s = (*port)["keys"][KeyActionOption(a)].value<std::string>()) p.keys[a] = *s;
        }
        constexpr const char* axes[] = {"left_stick", "right_stick", "left_trigger", "right_trigger"};
        for (size_t a = 0; a < 4; ++a) p.deadzone[a] = (*port)[axes[a]].value_or(p.deadzone[a]);
        p.rumble[0] = (*port)["rumble_low"].value_or(1.0); p.rumble[1] = (*port)["rumble_high"].value_or(1.0);
      }
    }
    if (!Valid(loaded.controls)) return false;
    if (auto v = tbl["fullscreen"].value<bool>()) loaded.fullscreen = *v;
    if (auto v = tbl["vsync"].value<bool>()) loaded.vsync = *v;
    if (auto v = tbl["d3d12_host_vsync"].value<bool>()) loaded.d3d12_host_vsync = *v;
    if (auto v = tbl["video_mode_refresh_rate"].value<double>()) loaded.video_mode_refresh_rate = *v;
    if (auto v = tbl["resolution_scale"].value<int>()) {
      loaded.resolution_scale = *v;
      loaded.draw_resolution_scale_x = *v;
      loaded.draw_resolution_scale_y = *v;
    }
    if (auto v = tbl["d3d12_allow_variable_refresh_rate_and_tearing"].value<bool>()) {
      loaded.d3d12_allow_variable_refresh_rate_and_tearing = *v;
    }
    if (auto v = tbl["anisotropic_override"].value<int>()) loaded.anisotropic_override = *v;
    if (auto v = tbl["msaa_samples"].value<int>()) loaded.msaa_samples = *v;
    if (auto v = tbl["transparency_antialiasing"].value<bool>()) loaded.transparency_antialiasing = *v;
    if (auto v = tbl["frame_rate"].value<int>()) {
      loaded.frame_rate = *v;
    } else if (tbl["unlock_fps"].value<bool>().value_or(false)) {
      loaded.frame_rate = 60;  // A configuration from before frame_rate, with the 60 fps option on.
    }
    if (auto v = tbl["brightness"].value<double>()) loaded.brightness = *v;
    if (auto v = tbl["internal_resolution_scale"].value<double>()) loaded.internal_resolution_scale = *v;
    if (auto v = tbl["max_frame_latency"].value<int>()) loaded.max_frame_latency = *v;
    if (auto v = tbl["triple_buffering"].value<bool>()) loaded.triple_buffering = *v;
    if (auto v = tbl["swap_post_effect"].value<std::string>()) loaded.swap_post_effect = *v;
    if (auto v = tbl["present_effect"].value<std::string>()) loaded.present_effect = *v;
    if (auto v = tbl["present_dither"].value<bool>()) loaded.present_dither = *v;
    if (auto v = tbl["show_fps_overlay"].value<bool>()) loaded.show_fps_overlay = *v;
    if (auto v = tbl["ultrawide_target_aspect"].value<double>()) loaded.ultrawide_target_aspect = *v;
    if (auto v = tbl["custom_game_root"].value<std::string>()) loaded.custom_game_root = *v;
    if (auto v = tbl["user_data_root"].value<std::string>()) loaded.user_data_root = *v;
    if (auto v = tbl["launcher_language"].value<std::string>()) loaded.launcher_language = *v;
    config = std::move(loaded);
    return true;
  } catch (const toml::parse_error&) {
    return false;
  }
}

bool ConfigManager::Save(const std::filesystem::path& toml_path, const GameConfig& config) {
  using namespace narutorise::controls;
  if (!Valid(config.controls)) return false;
  toml::table tbl;
  if (std::filesystem::exists(toml_path)) {
    try {
      std::ifstream input(toml_path, std::ios::binary);
      if (!input) return false;
      tbl = toml::parse(input);
      if(tbl["controls"]["version"].value_or(1)>1)return false;
    } catch (...) {
      return false;
    }
  }

  tbl.insert_or_assign("gpu_plugin", config.gpu_plugin);
  tbl.insert_or_assign("graphics_device", config.graphics_device);
  // The 60 fps option from before frame_rate is no longer an option of the
  // game: a configuration that still has it loses it on the first save.
  tbl.erase("unlock_fps");
  if (kNativeRendererBuilt) {
    tbl.insert_or_assign("renderer", "native");
    tbl.insert_or_assign("native_backend", config.native_backend);
    tbl.insert_or_assign("msaa_samples", config.msaa_samples);
    tbl.insert_or_assign("transparency_antialiasing", config.transparency_antialiasing);
    tbl.insert_or_assign("frame_rate", config.frame_rate);
    tbl.insert_or_assign("brightness", config.brightness);
    tbl.insert_or_assign("internal_resolution_scale", config.internal_resolution_scale);
    tbl.insert_or_assign("max_frame_latency", config.max_frame_latency);
    tbl.insert_or_assign("triple_buffering", config.triple_buffering);
  }
  tbl.insert_or_assign("d3d12_readback_resolve", config.d3d12_readback_resolve);
  tbl.insert_or_assign("skip_intro_videos", config.skip_intro_videos);
  tbl.insert_or_assign("user_language", config.user_language);
  tbl.insert_or_assign("translation_mod", config.translation_mod);
  tbl.insert_or_assign("log_verbose", config.log_verbose);
  tbl.insert_or_assign("log_level", config.log_level);
  tbl.insert_or_assign("protect_zero", config.protect_zero);
  tbl.insert_or_assign("input_backend", config.input_backend);
  tbl.insert_or_assign("mnk_mode", config.mnk_mode);
  tbl.insert_or_assign("mnk_mouse", config.mnk_mouse);
  tbl.insert_or_assign("mnk_sensitivity", config.mnk_sensitivity);
  for (std::size_t i = 0; i < kKeyActionCount; ++i) {
    tbl.insert_or_assign(KeyActionOption(i), config.key_binds[i]);
  }
  if (config.controls.version) {
    if (!tbl["controls"].is_table()) tbl.insert_or_assign("controls", toml::table{});
    auto& controls = *tbl["controls"].as_table();
    controls.insert_or_assign("version", config.controls.version);
    for (size_t i = 0; i < kPorts; ++i) {
      const auto& p = config.controls.ports[i];
      const auto name = "player" + std::to_string(i+1);
      if (!controls[name].is_table()) controls.insert_or_assign(name, toml::table{});
      auto& port = *controls[name].as_table();
      port.insert_or_assign("device", p.device); port.insert_or_assign("device_name", p.device_name);
      port.insert_or_assign("mouse", p.mouse); port.insert_or_assign("deadzones", p.deadzones);
      port.insert_or_assign("sensitivity", p.sensitivity);
      constexpr const char* axes[] = {"left_stick", "right_stick", "left_trigger", "right_trigger"};
      for (size_t a = 0; a < 4; ++a) port.insert_or_assign(axes[a], p.deadzone[a]);
      port.insert_or_assign("rumble_low", p.rumble[0]); port.insert_or_assign("rumble_high", p.rumble[1]);
      for (const char* bank : {"pad", "keys"}) if (!port[bank].is_table()) port.insert_or_assign(bank, toml::table{});
      for (size_t a = 0; a < kKeyActionCount; ++a) {
        port["pad"].as_table()->insert_or_assign(KeyActionOption(a), Encode(p.pad[a]));
        port["keys"].as_table()->insert_or_assign(KeyActionOption(a), p.keys[a]);
      }
    }
  }
  tbl.insert_or_assign("fullscreen", config.fullscreen);
  tbl.insert_or_assign("vsync", config.vsync);
  tbl.insert_or_assign("d3d12_host_vsync", config.d3d12_host_vsync);
  tbl.insert_or_assign("video_mode_refresh_rate", config.video_mode_refresh_rate);
  tbl.insert_or_assign("resolution_scale", config.resolution_scale);
  tbl.insert_or_assign("draw_resolution_scale_x", config.resolution_scale);
  tbl.insert_or_assign("draw_resolution_scale_y", config.resolution_scale);
  tbl.insert_or_assign("d3d12_allow_variable_refresh_rate_and_tearing",
                       config.d3d12_allow_variable_refresh_rate_and_tearing);
  tbl.insert_or_assign("anisotropic_override", config.anisotropic_override);
  tbl.insert_or_assign("swap_post_effect", config.swap_post_effect);
  tbl.insert_or_assign("present_effect", config.present_effect);
  tbl.insert_or_assign("present_dither", config.present_dither);
  tbl.insert_or_assign("show_fps_overlay", config.show_fps_overlay);
  tbl.insert_or_assign("ultrawide_target_aspect", config.ultrawide_target_aspect);
  if (!config.custom_game_root.empty()) {
    tbl.insert_or_assign("custom_game_root", config.custom_game_root);
  } else {
    tbl.erase("custom_game_root");
  }
  tbl.insert_or_assign("launcher_language", config.launcher_language);

  auto temporary = toml_path; temporary += ".write.tmp";
  std::ofstream out(temporary, std::ios::trunc);
  if (!out.is_open()) return false;
  out << "# narutorise - runtime configuration generated by Naruto: Rise of a Ninja Launcher\n\n";
  out << tbl << "\n";
  out.close();
  std::error_code error;
  if (!out) { std::filesystem::remove(temporary, error); return false; }
#ifdef _WIN32
  const bool saved = MoveFileExW(temporary.c_str(), toml_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  std::filesystem::rename(temporary, toml_path, error);
  const bool saved = !error;
#endif
  if (!saved) std::filesystem::remove(temporary, error);
  return saved;
}
