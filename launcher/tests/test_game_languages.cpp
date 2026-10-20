#include "game_languages.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include "localization.h"

using namespace launcher_ui;

namespace {
void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void Write(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary);
  file << text;
  Require(file.good(), "Cannot write fixture");
}

void Package(const std::filesystem::path& root, const std::string& id,
             const std::string& name, const std::string& json = R"({"15132":"OPCOES"})") {
  Write(root / "mods" / id / "mod.toml",
        "[mod]\ntype='translation'\nbase_language='en'\nname='" + name + "'\n");
  Write(root / "mods" / id / "textos.json", json);
}

const GameLanguageOption& Find(const GameLanguageList& list, const std::string& mod) {
  const auto it = std::find_if(list.options.begin(), list.options.end(),
                               [&](const auto& option) { return option.translation_mod == mod; });
  Require(it != list.options.end(), "Missing language option");
  return *it;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) return 2;
  try {
    const auto root = std::filesystem::u8path(argv[1]);
    Require(!std::filesystem::exists(root), "Use a new fixture directory");
    GameConfig config;
    config.user_language = 5;
    config.launcher_language = "pt_BR";
    Require(DiscoverTranslationMods(root).empty(), "A missing mods folder must be harmless");
    const auto official = GameLanguages(config, {});
    Require(official.options.size() == 5 && official.options[official.selected].language == 5,
            "Official language selection changed");

    Package(root, "pt-br", "Portuguese");
    Package(root, "pt-br-alt", "Portuguese", "\xEF\xBB\xBF{\"15132\":\"Alternativa\"}");
    Package(root, "bad-json", "Broken", "{");
    Package(root, "duplicate", "Broken", R"({"1":"a","1":"b"})");
    Package(root, "empty", "Empty", "{}");
    Package(root, "bad id", "Invalid folder");
    Package(root, "missing-texts", "Missing");
    std::filesystem::remove(root / "mods/missing-texts/textos.json");
    Package(root, "unsupported", "Unsupported");
    Write(root / "mods/unsupported/mod.toml", "[mod]\ntype='translation'\nbase_language='fr'\n");
    Package(root, "other-mod", "Other");
    Write(root / "mods/other-mod/mod.toml", "[mod]\ntype='texture'\nbase_language='en'\n");
    Package(root, "escape", "Escape");
    Write(root / "mods/escape/mod.toml",
          "[mod]\ntype='translation'\nbase_language='en'\ntexts='../outside.json'\n");
    Package(root, "bad-manifest", "Broken manifest");
    Write(root / "mods/bad-manifest/mod.toml", "[mod\n");
    auto mods = DiscoverTranslationMods(root);
    Require(mods.size() == 2 && mods[0].id == "pt-br" && mods[1].id == "pt-br-alt",
            "Discovery must accept valid packages, sort ties and reject invalid packages");
    auto languages = GameLanguages(config, mods);
    Require(languages.options.size() == 7 && languages.selected == 3 &&
                config.translation_mod.empty() && config.user_language == 5,
            "Installing a mod must not activate it");
    Require(Find(languages, "pt-br").label == "Portuguese" &&
                Find(languages, "pt-br-alt").label == "Portuguese",
            "Package labels must use manifest names without launcher suffixes");

    SelectGameLanguage(config, Find(languages, "pt-br"));
    Require(config.user_language == 1 && config.translation_mod == "pt-br" &&
                config.launcher_language == "pt_BR",
            "Selecting a translation must set its base without changing the interface language");
    const auto path = root / "narutorise.toml";
    Write(path, "update_data_root='custom-update'\n");
    Require(ConfigManager::Save(path, config), "Translation selection could not be saved");
    GameConfig reloaded;
    Require(ConfigManager::Load(path, reloaded), "Translation selection could not be reloaded");
    ReconcileGameLanguage(reloaded, mods);
    languages = GameLanguages(reloaded, mods);
    Require(languages.options[languages.selected].translation_mod == "pt-br" && reloaded.user_language == 1,
            "Reloaded selection must show the mod, not English");
    Localization::SetLanguageCode("pt_BR");
    const auto portuguese = GameLanguages(reloaded, mods);
    Require(portuguese.options[portuguese.selected].translation_mod == "pt-br",
            "Changing interface language changed the selected mod");

    SelectGameLanguage(reloaded, Find(languages, "pt-br-alt"));
    Require(reloaded.translation_mod == "pt-br-alt" && reloaded.user_language == 1,
            "Switching between translations failed");
    SelectGameLanguage(reloaded, languages.options[1]);  // French.
    Require(reloaded.translation_mod.empty() && reloaded.user_language == 4,
            "An official language must disable translation mods");
    Require(ConfigManager::Save(path, reloaded), "Official selection could not be saved");
    GameConfig official_reload;
    Require(ConfigManager::Load(path, official_reload) && official_reload.translation_mod.empty() &&
                official_reload.user_language == 4,
            "The previous translation survived an official language selection");
    SelectGameLanguage(reloaded, Find(languages, "pt-br"));
    SelectGameLanguage(reloaded, languages.options[0]);  // English also disables the mod.
    Require(reloaded.translation_mod.empty() && reloaded.user_language == 1,
            "English must disable translation mods");

    SelectGameLanguage(config, Find(languages, "pt-br"));
    std::filesystem::rename(root / "mods/pt-br", root / "removed");
    mods = DiscoverTranslationMods(root);
    ReconcileGameLanguage(config, mods);
    languages = GameLanguages(config, mods);
    Require(config.translation_mod.empty() && config.user_language == 1 &&
                languages.selected == 0 && languages.options.size() == 6 &&
                std::none_of(languages.options.begin(), languages.options.end(),
                             [](const auto& option) { return option.translation_mod == "pt-br"; }),
            "A removed mod must disappear from the list and automatically select English");
    Require(ConfigManager::Save(path, config), "Automatic English fallback could not be saved");
    GameConfig fallback_reload;
    Require(ConfigManager::Load(path, fallback_reload) && fallback_reload.translation_mod.empty() &&
                fallback_reload.user_language == 1,
            "Automatic English fallback did not persist");
    std::filesystem::rename(root / "removed", root / "mods/pt-br");
    mods = DiscoverTranslationMods(root);
    ReconcileGameLanguage(config, mods);
    languages = GameLanguages(config, mods);
    Require(config.translation_mod.empty() && languages.selected == 0 && languages.options.size() == 7,
            "Restoring a package must make it available without activating it");
    SelectGameLanguage(config, Find(languages, "pt-br"));
    Write(root / "mods/pt-br/textos.json", "{");
    ReconcileGameLanguage(config, DiscoverTranslationMods(root));
    Require(config.translation_mod.empty() && config.user_language == 1,
            "An invalid selected package must automatically select English");
    Package(root, "pt-br", "Portuguese");
    config.translation_mod = "pt-br";
    config.user_language = 4;
    ReconcileGameLanguage(config, DiscoverTranslationMods(root));
    Require(config.user_language == 1 && config.translation_mod == "pt-br",
            "A selected valid mod must use its English base");

    config.translation_mod.clear();
    config.user_language = 42;
    ReconcileGameLanguage(config, mods);
    languages = GameLanguages(config, mods);
    Require(languages.options[languages.selected].language == 42 &&
                languages.options[languages.selected].translation_mod.empty(),
            "Custom SDK language codes must remain selectable");
    if (argc == 3) {
      const auto installed = DiscoverTranslationMods(std::filesystem::u8path(argv[2]));
      Require(std::any_of(installed.begin(), installed.end(), [](const auto& mod) { return mod.id == "pt-br"; }),
              "The installed full Portuguese package was not discovered");
      std::cout << "Installed packages: " << installed.size() << '\n';
    }
    std::cout << "Game languages PASS: discovery, selection, persistence, fallback and restoration\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
