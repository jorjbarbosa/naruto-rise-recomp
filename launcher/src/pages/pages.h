#pragma once

#include <filesystem>
#include <string>

#include "config_manager.h"
#include "game_languages.h"

struct SDL_Window;

// The launcher pages, one per tab. Each one draws into the body area that
// LauncherUI leaves between the header and the footer.
namespace launcher_ui {

enum class Page { kPlay, kSettings, kGameFiles, kDlc };

// Where the game data and the game executable stand.
struct GameStatus {
  std::filesystem::path game_root;  // Empty when the game data was not found.
  bool executable_found = false;

  bool FilesFound() const { return !game_root.empty(); }
  bool Ready() const { return FilesFound() && executable_found; }
};

// What a page reads and changes.
struct PageContext {
  SDL_Window* window;
  const std::filesystem::path& base_dir;
  GameConfig& config;
  const GameStatus& game;
  std::string& status_message;
  bool& request_launch;
  Page& page;  // A page may send the user to another one.
  const std::vector<TranslationMod>& translation_mods;
};

void RenderPlayPage(PageContext& context);
void RenderSettingsPage(PageContext& context);
void RenderGameFilesPage(PageContext& context);
void RenderDlcPage(PageContext& context);

// The folder dialogs answer on another thread; their results are applied once
// per frame, before the pages are drawn.
void ApplyGameFolderSelection(PageContext& context);
void ApplyDlcFolderSelection(PageContext& context);

}  // namespace launcher_ui
