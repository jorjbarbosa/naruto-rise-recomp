#include "controls/config.h"
#include "menu/config_manager.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace narutorise::controls;
void Check(bool value, const char* why) {
  if (!value)
    throw std::runtime_error(why);
}
std::string Read(const std::filesystem::path& path) {
  std::ifstream f(path);
  return {std::istreambuf_iterator<char>(f), {}};
}
int main(int argc, char** argv) {
  if (argc != 2)
    return 2;
  try {
    const std::filesystem::path root(argv[1]);
    std::filesystem::create_directories(root);
    const auto path = root / "narutorise.toml";
    std::ofstream(path) << "personal_setting = 'keep'\nmnk_mode = true\nmnk_mouse = "
                           "true\nmnk_sensitivity = 2.5\nkeybind_a = 'E,,Backspace'\n";
    GameConfig config;
    Check(ConfigManager::Load(path, config), "legacy load");
    Check(config.controls.version == 0 && config.controls.ports[0].keys[0] == "E,,Backspace" &&
              config.controls.ports[0].sensitivity == 2.5 &&
              config.controls.ports[0].keys[16].find("MouseUp") != std::string::npos,
          "migration preserves alternatives and mouse");
    config.controls.version = 1;
    Assign(config.controls, 0, "keyboard", "Keyboard");
    Assign(config.controls, 1, "sdl:serial:test", "Controller");
    auto before = config.controls;
    auto proposed = config.controls;
    Assign(proposed, 2, "keyboard", "Keyboard");
    Check(config.controls == before, "cancel proposal must not mutate state");
    config.controls = proposed;
    Check(config.controls.ports[0].device.empty() &&
              config.controls.ports[2].device == "keyboard" &&
              config.controls.ports[0].keys == before.ports[0].keys,
          "transfer preserves source adjustments");
    auto& p = config.controls.ports[2];
    p.keys[0] = "LMB,E,Backspace";
    p.keys[1] = "Q,R";
    BindKey(p, 1, 1, "E");
    Check(p.keys[0] == "LMB,,Backspace" && p.keys[1] == "Q,E",
          "conflicts preserve other alternatives");
    Check(config.controls.ports[0] == proposed.ports[0], "other port isolation");
    {
      KeyBinds bank{};
      for (const auto* modifier : {"Shift", "Control", "Alt"}) {
        bank = {}; bank[0] = modifier;
        const auto chord = std::string(modifier) + "+G";
        Check(ModifierConflict(bank, chord), "new chord must not conflict with standalone modifier");
        bank[0] = "F5, " + chord;
        Check(ModifierConflict(bank, modifier), "new standalone modifier must not break an existing chord");
      }
      bank = {}; bank[0] = "Shift+Ctrl+G";
      Check(ModifierConflict(bank, "Control"), "Ctrl prefix alias is a modifier conflict");
      Check(ModifierConflict(bank, "Shift"), "combined chord retains every modifier");
      Check(!ModifierConflict(bank, "Alt") && !ModifierConflict(bank, "G"), "unrelated keys are allowed");
      bank = {};
      Check(ModifierConflict(bank, "Shift", "Shift+F5"), "global guide chord is protected");
      Check(ModifierConflict(bank, "Ctrl+G", "Control"), "Ctrl chord alias respects global guide modifier");
      Port replacement;
      replacement.keys = {}; replacement.keys[0] = "Shift";
      BindKey(replacement, 0, 0, "Shift+G");
      Check(!ModifierConflict(replacement.keys, "Shift+G"), "replacing the only standalone modifier is allowed");
    }
    p.keys[0] = "NumpadEnter,Backspace";
    p.keys[1] = "Shift+Ctrl+G";
    BindKey(p, 2, 0, "Return");
    BindKey(p, 3, 0, "Control+Shift+G");
    Check(p.keys[0] == ",Backspace" && p.keys[1].empty(),
          "semantic key aliases resolve conflicts locally");
    p.keys[0] = Join(std::vector<std::string>(40, "F5"));
    BindKey(p, 0, 39, "F6");
    Check(Alternatives(p.keys[0]).back() == "F6", "all existing alternatives remain editable");
    p.pad[0] = {SourceKind::Axis, 2, -1};
    p.pad[3] = {};
    p.deadzone = {0, .95, .2, .3};
    p.rumble = {0, .7};
    Check(ConfigManager::Save(path, config), "save");
    auto text = Read(path);
    Check(text.find("personal_setting") != std::string::npos, "unknown key preservation");
    GameConfig loaded;
    Check(ConfigManager::Load(path, loaded) && loaded.controls == config.controls,
          "four port roundtrip");
    auto invalid = config;
    invalid.controls.ports[0].deadzone[0] = 1;
    Check(!ConfigManager::Save(path, invalid) && Read(path) == text,
          "invalid deadzone must not save");
    invalid = config;
    invalid.controls.ports[0].sensitivity = std::numeric_limits<double>::quiet_NaN();
    Check(!ConfigManager::Save(path, invalid) && Read(path) == text, "NaN must not save");
    auto extremes = config;
    extremes.controls.ports[0].sensitivity = .01;
    extremes.controls.ports[0].deadzone = {0, .95, 0, .95};
    extremes.controls.ports[0].rumble = {0, 1};
    Check(Valid(extremes.controls), "minimum valid backend values");
    extremes.controls.ports[0].sensitivity = 10;
    Check(Valid(extremes.controls), "maximum valid backend sensitivity");
    for (double bad : {.009, 10.001, std::numeric_limits<double>::infinity()}) {
      invalid = config;
      invalid.controls.ports[0].sensitivity = bad;
      Check(!ConfigManager::Save(path, invalid) && Read(path) == text,
            "sensitivity boundaries preserve file");
    }
    for (double bad : {-.01, 1.01}) {
      invalid = config;
      invalid.controls.ports[0].rumble[0] = bad;
      Check(!ConfigManager::Save(path, invalid) && Read(path) == text,
            "motor boundaries preserve file");
    }
    invalid = config;
    invalid.controls.ports[0].pad[0] = {SourceKind::Axis, 6, 1};
    Check(!Valid(invalid.controls), "unsupported axis rejected");
    invalid = config;
    invalid.controls.ports[0].pad[0] = {SourceKind::Button, 3, 1};
    Check(!Valid(invalid.controls), "combined source bits rejected");
    invalid = config;
    invalid.controls.ports[0].device = invalid.controls.ports[2].device;
    Check(!ConfigManager::Save(path, invalid) && Read(path) == text,
          "duplicate assignment must not save");
    auto device = p.device;
    Restore(p);
    Check(p.device == device && p.keys == DefaultKeys() && p.pad == DefaultPad(),
          "restore both banks keeping device");
    const auto temporary = std::filesystem::path(path.string() + ".write.tmp");
    std::filesystem::create_directory(temporary);
    Check(!ConfigManager::Save(path, config) && Read(path) == text,
          "write failure preserves original file");
    std::filesystem::remove(temporary);
    std::ofstream(path) << "[controls]\nversion = 2\n";
    const auto future = Read(path);
    auto before_load = config;
    Check(!ConfigManager::Load(path, config) && config.controls == before_load.controls,
          "failed load keeps last effective state");
    Check(!ConfigManager::Save(path, config) && Read(path) == future,
          "future schema not downgraded");
    std::ofstream(path) << "broken = [";
    const auto broken = Read(path);
    Check(!ConfigManager::Save(path, config) && Read(path) == broken, "malformed config survives");
    Check(!ConfigManager::Save(root / "missing" / "narutorise.toml", config), "write failure");
    Check(Decode(Encode({SourceKind::Axis, 3, -1})) == Source{SourceKind::Axis, 3, -1},
          "signed source roundtrip");
    std::cout << "PASS: migration, alternatives, four ports, transactional save, limits, "
                 "transfer/reset isolation\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
