#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "config_manager.h"

namespace launcher_ui {

struct TranslationMod {
  std::string id;
  std::string name;
};

// Validate with the game's package reader, then retain only display metadata.
std::vector<TranslationMod> DiscoverTranslationMods(const std::filesystem::path& mods_dir);

struct GameLanguageOption {
  std::string label;
  std::uint32_t language;
  std::string translation_mod;
};

struct GameLanguageList {
  std::vector<GameLanguageOption> options;
  int selected = 0;
};

// Only installed, valid packages appear. Labels come directly from the manifest.
GameLanguageList GameLanguages(const GameConfig& config, const std::vector<TranslationMod>& mods);
void SelectGameLanguage(GameConfig& config, const GameLanguageOption& option);

// Keep a selected package on its English base. If it is no longer available,
// select English and clear the mod; restoring a package does not activate it.
void ReconcileGameLanguage(GameConfig& config, const std::vector<TranslationMod>& mods);

}  // namespace launcher_ui
