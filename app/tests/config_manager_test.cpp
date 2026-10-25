#include "menu/config_manager.h"

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

    const auto unicode_dir = root / std::filesystem::path(u8"instalação-設定");
    std::filesystem::create_directories(unicode_dir);
    const auto unicode_path = unicode_dir / "narutorise.toml";
    Write(unicode_path, "fullscreen = false\nlauncher_language = 'pt_BR'\nuser_language = 3\n"
                        "personal_setting = 'keep'\nuser_data_root = 'user'\n");
    GameConfig unicode_config;
    Require(ConfigManager::Load(unicode_path, unicode_config), "Unicode install path failed to load settings");
    Require(!unicode_config.fullscreen && unicode_config.launcher_language == "pt_BR" && unicode_config.user_language == 3,
            "Unicode install path reset user preferences");
    Require(ConfigManager::Save(unicode_path, unicode_config), "Unicode install path failed to save settings");
    std::ifstream unicode_file(unicode_path, std::ios::binary);
    const auto unicode_table = toml::parse(unicode_file);
    Require(unicode_table["personal_setting"].value<std::string>() == "keep" &&
                unicode_table["user_data_root"].value<std::string>() == "user",
            "Saving through a Unicode path erased unmanaged user settings");

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

    // The renderer and the native renderer's options: written only when the
    // executable carries the native renderer, and kept as they are otherwise.
    Write(path, "renderer = 'native'\nmsaa_samples = 8\nbrightness = 0.7\ninternal_resolution_scale = 1.5\n"
                "frame_rate = 120\ntransparency_antialiasing = true\nmax_frame_latency = 2\n"
                "triple_buffering = true\n");
    GameConfig native;
    Require(ConfigManager::Load(path, native), "Native renderer options failed to load");
    Require(native.renderer == "native" && native.msaa_samples == 8 && native.brightness == 0.7 &&
                native.internal_resolution_scale == 1.5 && native.frame_rate == 120 &&
                native.transparency_antialiasing && native.max_frame_latency == 2 && native.triple_buffering,
            "Native renderer options were not read");
    // The 60 fps option from before frame_rate opens at 60; the save removes
    // it, since the game no longer has it.
    Write(path, "frame_rate = 30\n");
    GameConfig original;
    Require(ConfigManager::Load(path, original) && original.frame_rate == 30, "frame_rate 30 was not read");
    Write(path, "unlock_fps = true\n");
    GameConfig unlocked;
    Require(ConfigManager::Load(path, unlocked) && unlocked.frame_rate == 60, "unlock_fps did not open at 60");
    unlocked.frame_rate = 144;
    Require(ConfigManager::Save(path, unlocked), "frame_rate failed to save");
    const auto rates = toml::parse_file(path.string());
    Require(!rates.contains("unlock_fps") && (!kNativeRendererBuilt || rates["frame_rate"].value<int>() == 144),
            "frame_rate was not saved as built or unlock_fps was kept");
    native.renderer = "xenos";
    native.msaa_samples = 2;
    Require(ConfigManager::Save(path, native), "Native renderer options failed to save");
    const auto saved = toml::parse_file(path.string());
    Require(saved["renderer"].value<std::string>() == "native",
            "The native-capable launcher must save the native renderer");
    Require(saved["msaa_samples"].value<int>() == (kNativeRendererBuilt ? 2 : 8),
            "Native renderer option was not saved as built");
    Write(path, "renderer = 'xenos'\nnative_backend = 'vulkan'\n");
    GameConfig migrated;
    Require(ConfigManager::Load(path, migrated) &&
                migrated.renderer == (kNativeRendererBuilt ? "native" : "xenos") &&
                migrated.native_backend == "vulkan",
            "Legacy Xenos selection did not migrate while preserving the native API");
    Require(ConfigManager::Save(path, migrated), "Migrated renderer failed to save");
    Require(toml::parse_file(path.string())["renderer"].value<std::string>() ==
                (kNativeRendererBuilt ? "native" : "xenos"),
            "Migrated renderer did not persist");
    for (const auto* api : {"d3d12", "vulkan"}) {
      if (!kNativeRendererBuilt) continue;
      migrated.native_backend = api;
      Require(ConfigManager::Save(path, migrated), "Native API failed to save");
      GameConfig restored;
      Require(ConfigManager::Load(path, restored) && restored.renderer == "native" &&
                  restored.native_backend == api, "Native API did not survive a round trip");
    }
    // The graphics card, for both renderers: read and saved by name.
    Write(path, "graphics_device = 'NVIDIA GeForce RTX 4060 Laptop GPU'\n");
    GameConfig device;
    Require(ConfigManager::Load(path, device) && device.graphics_device == "NVIDIA GeForce RTX 4060 Laptop GPU",
            "graphics_device was not read");
    device.graphics_device.clear();
    Require(ConfigManager::Save(path, device) &&
                toml::parse_file(path.string())["graphics_device"].value<std::string>() == "",
            "graphics_device was not saved");
    // user_data_root is read for the command line and kept as written.
    Write(path, "user_data_root = 'user'\n");
    GameConfig user_root;
    Require(ConfigManager::Load(path, user_root) && user_root.user_data_root == "user",
            "user_data_root was not read");
    Require(ConfigManager::Save(path, user_root) &&
                toml::parse_file(path.string())["user_data_root"].value<std::string>() == "user",
            "Saving changed user_data_root");
    Write(path, "renderer = 'vulkan'\n");
    GameConfig unknown;
    Require(ConfigManager::Load(path, unknown) && unknown.renderer == "native",
            "An unknown renderer must fall back to the native renderer, the default");
    Write(path, "launcher_language = 'en'\n");
    GameConfig no_renderer;
    Require(ConfigManager::Load(path, no_renderer) && no_renderer.renderer == "native",
            "A configuration without a renderer must open with the native renderer");
    // Keyboard and mouse: a configuration from before the key options opens
    // with the default layout and writes it on the first save.
    Write(path, "mnk_mode = true\n");
    GameConfig keys;
    Require(ConfigManager::Load(path, keys) && keys.mnk_mode, "mnk_mode was not read");
    Require(keys.mnk_mouse && keys.mnk_sensitivity == 1.0 && keys.key_binds == DefaultKeyBinds(),
            "Missing key options must keep the defaults");
    Require(ConfigManager::Save(path, keys), "Key options failed to save");
    {
      const auto table = toml::parse_file(path.string());
      for (std::size_t i = 0; i < kKeyActionCount; ++i) {
        Require(table[KeyActionOption(i)].value<std::string>() == DefaultKeyBinds()[i],
                "The default layout was not written");
      }
      Require(table["mnk_mouse"].value<bool>() == true && table["mnk_sensitivity"].value<double>() == 1.0,
              "The mouse options were not written");
    }
    // Values written by hand stay as they are: three keys, a combination, a
    // sensitivity outside the slider, and the Guide button the launcher does
    // not show.
    Write(path, "mnk_mouse = false\nmnk_sensitivity = 5.0\nkeybind_a = 'Space,Return,K'\n"
                "keybind_dpad_up = 'Shift+Up'\nkeybind_b = ',Backspace'\nkeybind_guide = 'G'\n");
    GameConfig manual;
    Require(ConfigManager::Load(path, manual), "Hand-written key options failed to load");
    Require(!manual.mnk_mouse && manual.mnk_sensitivity == 5.0, "Hand-written mouse options were not read");
    Require(manual.key_binds[0] == "Space,Return,K" && manual.key_binds[20] == "Shift+Up" &&
                manual.key_binds[1] == ",Backspace",
            "Hand-written keys were not read as written");
    manual.key_binds[4] = "Q,Alt";
    Require(ConfigManager::Save(path, manual), "Hand-written key options failed to save");
    {
      const auto table = toml::parse_file(path.string());
      Require(table["keybind_a"].value<std::string>() == "Space,Return,K" &&
                  table["keybind_dpad_up"].value<std::string>() == "Shift+Up" &&
                  table["keybind_b"].value<std::string>() == ",Backspace" &&
                  table["keybind_left_shoulder"].value<std::string>() == "Q,Alt" &&
                  table["keybind_guide"].value<std::string>() == "G",
              "Saving changed hand-written keys");
      Require(table["mnk_mouse"].value<bool>() == false && table["mnk_sensitivity"].value<double>() == 5.0,
              "Saving changed hand-written mouse options");
    }
    std::cout << "Game languages persist; launcher language stays independent; "
                 "legacy, custom and invalid values handled; renderer options "
              << (kNativeRendererBuilt ? "saved" : "kept") << "; key options kept and defaulted\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
