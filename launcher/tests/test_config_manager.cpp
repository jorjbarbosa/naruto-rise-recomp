#include "config_manager.h"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <toml++/toml.hpp>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void Write(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path);
  out << text;
  Require(out.good(), "Could not write test fixture");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  try {
    const auto root = std::filesystem::u8path(argv[1]);
    std::filesystem::create_directories(root);
    const auto path = root / "narutorise.toml";

    Write(path, "launcher_language = 'pt_BR'\n");
    GameConfig legacy;
    Require(ConfigManager::Load(path, legacy), "Legacy configuration failed to load");
    Require(legacy.user_language == 1, "Missing game language must default to English");
    Require(legacy.launcher_language == "pt_BR", "Launcher language was changed");
    Require(ConfigManager::Save(path, legacy), "Legacy configuration failed to save");
    Require(toml::parse_file(path.string())["user_language"].value<int>() == 1,
            "English default was not written to TOML");

    for (const std::uint32_t code : {1u, 4u, 3u, 5u, 6u, 2u}) {
      Write(path, "user_language = " + std::to_string(code) +
                      "\nlauncher_language = 'pt_BR'\nupdate_data_root = 'D:/Title Update'\n");
      GameConfig config;
      Require(ConfigManager::Load(path, config), "Game language failed to load");
      Require(config.user_language == code, "Existing game language was not preserved");
      config.launcher_language = "en";
      Require(ConfigManager::Save(path, config), "Game language failed to save");
      const auto table = toml::parse_file(path.string());
      Require(table["user_language"].value<std::uint32_t>() == code,
              "Changing launcher language changed the game's language");
      Require(table["update_data_root"].value<std::string>() == "D:/Title Update",
              "Saving erased an unmanaged runtime setting");

      config.user_language = code == 5 ? 1 : 5;
      config.launcher_language = "pt_BR";
      Require(ConfigManager::Save(path, config), "Selected game language failed to save");
      GameConfig reloaded;
      Require(ConfigManager::Load(path, reloaded), "Saved game language failed to reload");
      Require(reloaded.user_language == config.user_language, "Game selection did not persist");
      Require(reloaded.launcher_language == "pt_BR", "Game selection changed launcher language");
    }

    for (const auto* value : {"'Spanish'", "-1", "4294967296"}) {
      Write(path, std::string("user_language = ") + value + "\n");
      GameConfig config;
      Require(ConfigManager::Load(path, config), "Invalid language prevented TOML loading");
      Require(config.user_language == 1, "Invalid language must keep the English default");
    }

    const auto new_path = root / "new.toml";
    for (const auto* language : {"en", "pt_BR", "fr", "de", "es", "it", "ru"}) {
      GameConfig config;
      config.launcher_language = language;
      config.user_language = 5;
      Require(ConfigManager::Save(new_path, config), "Interface language failed to save");
      GameConfig reloaded;
      Require(ConfigManager::Load(new_path, reloaded), "Interface language failed to reload");
      Require(reloaded.launcher_language == language, "Interface language did not persist");
      Require(reloaded.user_language == 5, "Interface language changed game language");
    }
    Require(ConfigManager::Save(new_path, GameConfig{}), "New configuration failed to save");
    Require(toml::parse_file(new_path.string())["user_language"].value<int>() == 1,
            "New configuration must write the English default");
    std::cout << "Game languages persist; launcher language stays independent; "
                 "legacy, custom and invalid values handled\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
