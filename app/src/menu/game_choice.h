// The system file picker and asynchronous game identification. The future owns
// its work (no detached callbacks into an app that may have closed).
#pragma once
#include "game_image.h"
#include <chrono>
#include <filesystem>
#include <future>
#include <optional>
#include <string>

namespace narutorise::menu {
struct FileChoice {
  std::filesystem::path path;
  std::string error;
};
FileChoice ChooseFile(void* owner, bool folder = false);
bool SaveGameRoot(const std::filesystem::path& config, const std::filesystem::path& root,
                  std::string& error);
class GameChoice {
 public:
  bool Start(const std::filesystem::path& path);
  bool Busy() const { return result_.valid(); }
  bool Slow() const;
  struct Result { std::filesystem::path root; game_image::Identity game; };
  std::optional<Result> Poll();
 private:
  std::chrono::steady_clock::time_point started_{};
  std::future<Result> result_;
};
}  // namespace narutorise::menu
