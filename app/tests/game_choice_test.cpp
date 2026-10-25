// Identification must not block UI work; a default.xex selects its directory.
// Persistence must preserve unknown TOML keys and reject malformed files.
#include "menu/game_choice.h"
#include <fstream>
#include <iostream>
#include <thread>
#include <toml++/toml.hpp>
int main(int argc, char** argv) {
  if (argc != 3) return 2;
  using namespace narutorise::menu;
  const auto temp = std::filesystem::path(argv[1]);
  std::filesystem::create_directories(temp);
  const auto config = temp / "config.toml";
  std::ofstream(config) << "unknown = 42\n[other]\nvalue = 'preserved'\n";
  std::string error;
  const auto root = std::filesystem::path(argv[2]);
  if (!SaveGameRoot(config, root, error)) return 3;
  auto table = toml::parse_file(config.string());
  if (table["unknown"].value<int>() != 42 || table["other"]["value"].value<std::string>() != "preserved") return 4;
  std::ofstream(config) << "invalid = [";
  if (SaveGameRoot(config, root, error)) return 5;
  GameChoice choice;
  if (!choice.Start(root / "default.xex") || choice.Start(root)) return 6;
  int ticks = 0;
  while (choice.Busy()) {
    ++ticks;
    if (auto result = choice.Poll()) {
      if (result->game.status != narutorise::game_image::Status::kReady ||
          result->root != std::filesystem::weakly_canonical(root)) return 7;
    }
    if (ticks > 10000) return 8;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!choice.Start(temp / "missing.iso")) return 9;
  for (;;) {
    if (auto result = choice.Poll()) {
      if (result->game.status != narutorise::game_image::Status::kUnreadable) return 10;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::cout << "PASS: TOML preservation, parse failure, async identification, XEX normalization, missing file\n";
}
