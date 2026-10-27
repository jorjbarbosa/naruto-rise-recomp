// Real RmlUi documents, settings and asynchronous content work over a CPU sink.
// This verifies menu persistence/navigation independently of GPU presentation.
#include "menu/ui.h"
#include "menu/settings.h"
#include "menu/dlc.h"
#include "menu/game_choice.h"
#include "data_paths.h"
#include <RmlUi/Core.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/ui_event.h>
#include <rex/filesystem/devices/stfs_xbox.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <toml++/toml.hpp>
#include <SDL3/SDL.h>
namespace fs = std::filesystem;
namespace {
int checks = 0;
void Check(bool value, const char* message) {
  ++checks; if (!value) throw std::runtime_error(message);
}
void Write(const fs::path& path, const std::string& value) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary); output << value;
  if (!output) throw std::runtime_error("Fixture write failed");
}
std::string Read(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Fixture read failed");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
struct Texture : rex::ui::ImmediateTexture {
  Texture(uint32_t w, uint32_t h) : ImmediateTexture(w, h) {}
};
struct CpuDrawer : rex::ui::ImmediateDrawer {
  std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(uint32_t w, uint32_t h,
      rex::ui::ImmediateTextureFilter, bool, const uint8_t*) override {
    return std::make_unique<Texture>(w, h);
  }
  void BeginDrawBatch(const rex::ui::ImmediateDrawBatch&) override {}
  void Draw(const rex::ui::ImmediateDraw&) override {}
  void EndDrawBatch() override {}
};
Rml::Element* Element(const char* id) {
  auto* context = Rml::GetContext("game");
  for (int i = 0; context && i < context->GetNumDocuments(); ++i) {
    auto* document = context->GetDocument(i);
    if (document->IsVisible()) if (auto* element = document->GetElementById(id)) return element;
  }
  return nullptr;
}
}
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  try {
    const auto root = fs::absolute(fs::path(std::u8string(argv[1], argv[1] + std::char_traits<char>::length(argv[1]))));
    Check(!fs::exists(root), "Fixture must be new");
    for (const char* hint : {SDL_HINT_JOYSTICK_HIDAPI, SDL_HINT_JOYSTICK_DIRECTINPUT,
         SDL_HINT_JOYSTICK_RAWINPUT, SDL_HINT_JOYSTICK_WGI, SDL_HINT_JOYSTICK_GAMEINPUT, SDL_HINT_XINPUT_ENABLED})
      SDL_SetHintWithPriority(hint, "0", SDL_HINT_OVERRIDE);
    const auto install = root / "distribution";
    const auto active = root / fs::path(u8"data 日本語/narutorise.toml");
    const std::string legacy = "unknown_option = 42\nshow_startup_menu = false\n";
    Write(install / "narutorise.toml", legacy);
    Write(active, "unknown_option = 84\nshow_startup_menu = false\nlauncher_language = 'en'\n");
    CpuDrawer drawer;
    narutorise::menu::Ui ui;
    Check(ui.Initialize(drawer, nullptr), "Real menu documents initialize without a GPU");
    rex::ui::AppUIDrawContext context(1280, 720);
    const auto frame = [&] { ui.Draw(context); };
    narutorise::menu::Settings settings;
    settings.Initialize(install, false, active);
    settings.Open(ui, 4, false, {}, {}, {}); frame();
    auto* startup = Element("show_startup_menu");
    Check(startup && startup->Focus(), "Advanced option receives keyboard focus");
    auto* rml = Rml::GetContext("game");
    rml->ProcessKeyDown(Rml::Input::KI_RIGHT, 0); rml->ProcessKeyUp(Rml::Input::KI_RIGHT, 0); frame();
    toml::table table;
    { std::ifstream input(active, std::ios::binary); table = toml::parse(input); }
    Check(table["show_startup_menu"].value<bool>() == true &&
          table["unknown_option"].value<int>() == 84, "UI edit writes active Unicode TOML and preserves unknown keys");
    Check(Read(install / "narutorise.toml") == legacy, "Legacy distribution TOML remains byte-identical");
    settings.Reload();
    Check(settings.config().show_startup_menu, "Reload reads the same active TOML");
    std::string save_error;
    Check(narutorise::menu::SaveGameRoot(active, root / "external game.iso", save_error),
          "Game selection writes the passed active Unicode config");
    settings.Reload();
    Check(!settings.config().custom_game_root.empty() && Read(install / "narutorise.toml") == legacy,
          "Game selection reload preserves legacy TOML");
    narutorise::data::DataPaths storage;
    storage.data_root = active.parent_path(); storage.distribution_dir = install;
    storage.mods_dir = storage.data_root / "mods";
    Write(storage.mods_dir / "pt-br/mod.toml", "[mod]\ntype='translation'\nbase_language='en'\nname='Active translation'\n");
    Write(storage.mods_dir / "pt-br/textos.json", "{\"15132\":\"ACTIVE MOD\"}");
    Write(install / "mods/pt-br/mod.toml", "[mod]\ntype='translation'\nbase_language='en'\nname='Installation decoy'\n");
    Write(install / "mods/pt-br/textos.json", "{\"15132\":\"WRONG MOD\"}");
    int location_requests = 0;
    auto last_mode = narutorise::data::Mode::kDefault;
    auto choose = [&](narutorise::data::Mode mode) { ++location_requests; last_mode = mode; };
    settings.SetDataFolder(storage, {}, choose);
    settings.Open(ui, 0, false, {}, {}, {}); frame();
    auto* language = Element("user_language");
    Check(language && language->Focus(), "Game language receives focus");
    for (int i = 0; i < 5; ++i) {
      rml->ProcessKeyDown(Rml::Input::KI_RIGHT, 0); rml->ProcessKeyUp(Rml::Input::KI_RIGHT, 0); frame();
    }
    Check(settings.config().translation_mod == "pt-br" && settings.config().user_language == 1 &&
          Element("user_language")->GetInnerRML().find("Active translation") != std::string::npos,
          "Menu discovers and selects only the active data mod, excluding installation decoy");
    settings.Reload();
    { std::ifstream input(active, std::ios::binary); table = toml::parse(input); }
    Check(table["translation_mod"].value<std::string>() == "pt-br" &&
          table["unknown_option"].value<int>() == 84 && settings.config().translation_mod == "pt-br",
          "Selected translation persists in active TOML and reload");
    settings.SetPendingDataLocation(root / "next location");
    settings.Open(ui, 0, false, {}, {}, {}); frame();
    Check(Element("user_language")->GetInnerRML().find("Active translation") != std::string::npos,
          "Pending data choice keeps menu mods on the active snapshot");
    settings.SetPendingDataLocation({});
    settings.Open(ui, 4, false, {}, {}, {}); frame();
    Check(Element("data-active") && Element("data-open"), "Advanced exposes the active folder and open action");
    const auto activate = [&](const char* id) {
      auto* option = Element(id); Check(option && option->Focus(), "Storage action is keyboard-focusable");
      rml->ProcessKeyDown(Rml::Input::KI_RETURN, 0); rml->ProcessKeyUp(Rml::Input::KI_RETURN, 0); frame();
    };
    activate("data-choose"); Check(location_requests == 1 && last_mode == narutorise::data::Mode::kCustom, "Choose action requests custom mode");
    activate("data-portable"); Check(location_requests == 2 && last_mode == narutorise::data::Mode::kPortable, "Portable action requests portable mode");
    activate("data-default"); Check(location_requests == 3 && last_mode == narutorise::data::Mode::kDefault, "Restore action requests default mode");
    const auto next = root / "next location";
    settings.SetPendingDataLocation(next); frame();
    Check(Element("data-pending") && Element("data-pending")->IsClassSet("modified") &&
          Element("data-active")->GetInnerRML().find("data") != std::string::npos,
          "Pending and active locations are separately visible");
    auto* toggle = Element("show_startup_menu"); Check(toggle && toggle->Focus(), "Settings remain editable while a selection is pending");
    rml->ProcessKeyDown(Rml::Input::KI_RIGHT, 0); rml->ProcessKeyUp(Rml::Input::KI_RIGHT, 0); frame();
    { std::ifstream input(active, std::ios::binary); table = toml::parse(input); }
    Check(table["show_startup_menu"].value<bool>() == false && !fs::exists(next / "narutorise.toml"),
          "Pending selection keeps writes in the active TOML until restart");
    storage.user_source = narutorise::data::Source::kConfig;
    storage.user_data_root = root / "explicit saves";
    settings.SetDataFolder(storage, {}, choose);
    settings.Open(ui, 4, false, {}, {}, {}); frame();
    Check(Element("data-guest") && Element("data-guest")->GetInnerRML().find("explicit saves") != std::string::npos,
          "Explicit guest storage is shown separately for backup and remains in place");
    storage.session_override = true;
    settings.SetDataFolder(storage, {}, choose);
    settings.Open(ui, 4, false, {}, {}, {}); frame();
    activate("data-choose"); activate("data-portable"); activate("data-default");
    Check(location_requests == 3 && !Element("data-pending"), "Session override blocks every storage selection action");
    for (const auto& language : Localization::GetLanguages()) {
      Localization::SetLanguage(language.language);
      settings.SetPendingDataLocation({}); frame();
      const auto expected = Rml::StringUtilities::EncodeRml(narutorise::menu::Text("storage-choose"));
      Check(!expected.empty() && Element("data-choose")->GetInnerRML().find(expected) != std::string::npos &&
            *narutorise::menu::Text("storage-error") && *narutorise::menu::Text("storage-error-help") &&
            *narutorise::menu::Text("storage-override") && *narutorise::menu::Text("storage-guest-help"), "Storage actions and errors are available in this language");
    }
    Localization::SetLanguage(Localization::Language::English);
    ui.HidePanel();
    const auto staging = root / "selected staging";
    const auto user = root / "effective guest";
    fs::create_directories(staging);
    rex::filesystem::StfsHeader package{};
    package.header.magic = rex::filesystem::XContentPackageType::kLive;
    package.metadata.set_display_name(rex::system::XLanguage::kEnglish, u"Storage test DLC");
    std::ofstream(staging / "package", std::ios::binary).write(reinterpret_cast<const char*>(&package), sizeof(package));
    narutorise::menu::Dlc dlc;
    dlc.Open(ui, staging, user, nullptr, [](auto action) { action(); }, {}); frame();
    auto* pending = Element("staged-1");
    Check(pending && pending->GetInnerRML().find("Storage test DLC") != std::string::npos,
          "DLC page enumerates the explicit staging folder");
    const auto loose = root / "selected loose";
    Write(loose / "content.bin", "loose content");
    dlc.InstallFolder(loose); dlc.Wait(); dlc.Poll(); frame();
    Check(Read(user / "0000000000000000/555307E5/00000002/selected loose/content.bin") == "loose content",
          "Menu asynchronous installation uses the effective guest root");
    Check(Read(loose / "content.bin") == "loose content", "Menu preserves source content");
    ui.HideDialog(); ui.HidePanel();
    dlc.Open(ui, {}, user, nullptr, [](auto action) { action(); }, {}); frame();
    Check(!Element("staged-1"), "Disabled source does not enumerate default staging");
    ui.Shutdown();
    std::cout << "PASS: " << checks << " checks; real menu events, active TOML, explicit DLC staging, guest root, originals\n";
  } catch (const std::exception& error) {
    std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n'; return 1;
  }
}
