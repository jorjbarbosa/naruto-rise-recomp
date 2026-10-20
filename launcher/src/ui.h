#pragma once

#include <filesystem>
#include <string>

#include "config_manager.h"
#include "pages/pages.h"

struct SDL_Window;
struct ID3D11ShaderResourceView;

// The launcher window: the cover on the left; on the right the header with the
// tabs and the language, the page of the selected tab and the footer.
class LauncherUI {
 public:
  struct Cover {
    ID3D11ShaderResourceView* texture = nullptr;  // Null draws a gradient with the title.
    int width = 0;
    int height = 0;
  };

  LauncherUI(SDL_Window* window, std::filesystem::path base_dir, Cover cover);

  static void SetupTheme();
  void Render(GameConfig& config, bool& request_launch, std::string& status_message);
  void RefreshTranslationMods();

 private:
  void RenderCover(float width, float height) const;
  void RenderHeader(GameConfig& config);
  void RenderPage(launcher_ui::PageContext& context) const;

  SDL_Window* window_;
  std::filesystem::path base_dir_;
  Cover cover_;
  std::string version_;
  launcher_ui::Page page_ = launcher_ui::Page::kPlay;
  std::vector<launcher_ui::TranslationMod> translation_mods_;
};
