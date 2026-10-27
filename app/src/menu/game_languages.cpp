#include "game_languages.h"

#include <algorithm>
#include <cstdio>

#include "localization.h"
#include "mods/translation_data.h"

namespace launcher_ui {
std::vector<TranslationMod> DiscoverTranslationMods(const std::filesystem::path& mods_dir) {
  std::vector<TranslationMod> mods;
  std::error_code error;
  std::filesystem::directory_iterator it(mods_dir, error), end;
  while (!error && it != end) {
    const auto& entry = *it;
    std::error_code entry_error;
    if (entry.is_directory(entry_error)) {
      // Validate the UTF-8 folder name before the package reader converts it
      // through the host code page. Only ASCII mod IDs are supported by the game.
      const auto name = entry.path().filename().u8string();
      const std::string id(name.begin(), name.end());
      if (narutorise::translation::ValidModId(id)) {
        narutorise::translation::Package package;
        std::string reason;
        if (narutorise::translation::LoadPackage(entry.path(), package, reason) && !package.texts.empty()) {
          mods.push_back({package.id, package.name.empty() ? package.id : package.name});
        }
      }
    }
    it.increment(error);
  }
  std::sort(mods.begin(), mods.end(), [](const auto& a, const auto& b) {
    return a.name == b.name ? a.id < b.id : a.name < b.name;
  });
  return mods;
}

GameLanguageList GameLanguages(const GameConfig& config, const std::vector<TranslationMod>& mods) {
  constexpr struct {
    std::uint32_t code;
    TextId label;
  } official[] = {
      {1, TextId::GameLanguageEnglish}, {4, TextId::GameLanguageFrench},
      {3, TextId::GameLanguageGerman}, {5, TextId::GameLanguageSpanish},
      {6, TextId::GameLanguageItalian},
  };
  GameLanguageList list;
  bool known_language = false;
  for (const auto& language : official) {
    if (config.user_language == language.code) {
      list.selected = static_cast<int>(list.options.size());
      known_language = true;
    }
    list.options.push_back({Tr(language.label), language.code, {}});
  }
  if (!known_language) {
    char label[128];
    std::snprintf(label, sizeof(label), Tr(TextId::GameLanguageCustom), config.user_language);
    list.selected = static_cast<int>(list.options.size());
    list.options.push_back({label, config.user_language, {}});
  }
  for (const auto& mod : mods) {
    if (config.translation_mod == mod.id) {
      list.selected = static_cast<int>(list.options.size());
    }
    list.options.push_back({mod.name, 1, mod.id});
  }
  return list;
}

void SelectGameLanguage(GameConfig& config, const GameLanguageOption& option) {
  config.user_language = option.language;
  config.translation_mod = option.translation_mod;
}

void ReconcileGameLanguage(GameConfig& config, const std::vector<TranslationMod>& mods) {
  if (config.translation_mod.empty()) return;
  config.user_language = 1;
  if (std::none_of(mods.begin(), mods.end(), [&](const auto& mod) { return mod.id == config.translation_mod; }))
    config.translation_mod.clear();
}

}  // namespace launcher_ui
