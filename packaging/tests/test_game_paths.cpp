#include "game_paths.h"

#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
  namespace fs = std::filesystem;
  if (argc != 2) return 2;
  const fs::path root = fs::u8path(argv[1]);
  auto require = [](bool condition) {
    if (!condition) throw std::runtime_error("game discovery assertion failed");
  };
  auto marker = [](const fs::path& folder) {
    fs::create_directories(folder);
    std::ofstream(folder / "default.xex") << "synthetic marker";
  };
  try {
    const fs::path base = root / "installed";
    fs::create_directories(base / "game");
    require(game_paths::Find(base, "").empty());
    marker(base / "game_root");
    require(game_paths::Find(base, "") == fs::canonical(base / "game_root"));
    marker(base / "game");
    require(game_paths::Find(base, "") == fs::canonical(base / "game"));
    marker(root / "custom");
    require(game_paths::Find(base, (root / "custom").string()) == fs::canonical(root / "custom"));
    require(game_paths::Find(base, (root / "missing").string()) == fs::canonical(base / "game"));
    require(fs::exists(base / "game_root/default.xex"));
    std::cout << "game preferred; custom and legacy discovery preserved; no data renamed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
