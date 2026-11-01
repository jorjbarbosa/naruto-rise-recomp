// Settings shared by the setup screen and the overlay. Reuses the launcher's
// configuration, language packages and key binding rules, without its ImGui UI.
#pragma once
#include "menu/panel.h"
#include "data_paths.h"
#include "../menu/config_manager.h"
#include <filesystem>
#include <functional>
#include <unordered_set>
#include <optional>
namespace narutorise::menu {
class Ui;
class Settings {
 public:
  struct MenuOptions { std::string key = "F1", chord = "Back+Start"; bool shaders = true; };
  const MenuOptions& menu_options() const { return menu_options_; }
  void SetUtilities(std::function<void()> shortcut, std::function<void()> updates, std::function<void()> game_files = {}) {
    shortcut_ = std::move(shortcut); updates_ = std::move(updates); game_files_ = std::move(game_files);
  }
  void Initialize(std::filesystem::path directory, bool native_controls = true,
                  std::filesystem::path config_path = {});
  void Open(Ui& ui, int tab, bool in_game, std::function<void()> choose_game,
            std::function<void(const std::string&)> changed, std::function<void()> closed);
  void Reload();
  void SetDataFolder(data::DataPaths active, std::optional<std::filesystem::path> pending,
                     std::function<void(data::Mode)> select) {
    data_paths_ = std::move(active); pending_data_root_ = std::move(pending);
    select_data_location_ = std::move(select);
  }
  void SetPendingDataLocation(std::optional<std::filesystem::path> pending);
  const GameConfig& config() const { return config_; }
  struct LiveResult { uint64_t revision = 0; bool applied = false; };
  void SetLiveSettings(std::function<uint64_t(const std::string&, const GameConfig&)> request,
                       std::function<LiveResult()> result) { request_live_ = std::move(request); result_live_ = std::move(result); }
  void PollLive();
  // The answer of the crash report question at the first start, saved with
  // crash_reporting; false when the configuration could not be written.
  bool AnswerCrashReporting(bool consent);
  // Before runtime creation only; runtime edits use the frame-boundary queue.
  bool ApplyBeforeGame() const;
 private:
  std::vector<PanelRow> Rows(int group);
  void Change(const std::string& key, std::function<void(GameConfig&)> change);
  void CommitChange(const std::string& key, GameConfig next, std::string focus = {});
  bool Save(const GameConfig& config);
  void Redraw(std::string focus = {});
  void Controls();
  void SaveMenuOptions(const std::string& key, MenuOptions next);
  std::filesystem::path directory_;
  std::filesystem::path config_path_;
  std::optional<data::DataPaths> data_paths_;
  std::optional<std::filesystem::path> pending_data_root_;
  std::function<void(data::Mode)> select_data_location_;
  GameConfig config_, initial_;
  std::unordered_set<std::string> edited_flags_;
  MenuOptions menu_options_, initial_menu_options_;
  std::function<void()> shortcut_, updates_, game_files_;
  Ui* ui_ = nullptr;
  int group_ = 0;
  bool in_game_ = false;
  bool load_failed_ = false;
  bool controls_enabled_ = true;
  std::function<void()> choose_game_, closed_;
  std::function<void(const std::string&)> changed_;
  std::function<uint64_t(const std::string&, const GameConfig&)> request_live_;
  std::function<LiveResult()> result_live_;
  std::optional<GameConfig> pending_config_;
  std::string pending_key_;
  uint64_t pending_ticket_ = 0, panel_revision_ = 0;
};
}  // namespace narutorise::menu
