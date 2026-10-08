#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct GameConfig {
  std::string gpu_plugin = "xenos";
  bool d3d12_readback_resolve = true;
  bool skip_intro_videos = true;

  // Game language: 1 = English, 4 = French, 3 = German, 5 = Spanish, 6 = Italian.
  std::uint32_t user_language = 1;

  bool log_verbose = false;
  std::string log_level = "info";

  bool protect_zero = false;

  std::string input_backend = "sdl";
  bool mnk_mode = false;

  bool fullscreen = true;
  bool vsync = true;
  bool d3d12_host_vsync = true;
  double video_mode_refresh_rate = 60.0;
  int resolution_scale = 2;
  int draw_resolution_scale_x = 2;
  int draw_resolution_scale_y = 2;
  bool d3d12_allow_variable_refresh_rate_and_tearing = true;

  int anisotropic_override = 5;

  std::string swap_post_effect = "fxaa";
  std::string present_effect = "cas";
  bool present_dither = false;
  bool show_fps_overlay = false;

  // 0.0 = 16:9 native, 2.3889 = 21:9, 3.5556 = 32:9
  double ultrawide_target_aspect = 2.3889;

  std::string custom_game_root = "";

  // Launcher interface: en (default), pt_BR, fr, de, es, it, ru.
  std::string launcher_language = "en";
};

class ConfigManager {
 public:
  static bool Load(const std::filesystem::path& toml_path, GameConfig& config);
  static bool Save(const std::filesystem::path& toml_path, const GameConfig& config);
};
