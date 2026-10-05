#pragma once

#include "config_manager.h"
#include <filesystem>
#include <string>

struct SDL_Window;
struct ID3D11ShaderResourceView;

class LauncherUI {
 public:
  static void SetupTheme();
  static bool Render(SDL_Window* window,
                     const std::filesystem::path& base_dir,
                     GameConfig& config,
                     bool& request_launch,
                     std::string& status_message,
                      ID3D11ShaderResourceView* cover_texture,
                      int cover_width = 0, int cover_height = 0);
};
