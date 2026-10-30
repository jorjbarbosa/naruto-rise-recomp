// narutorise - the game's own options, between its Options screen (the menus'
// script, game_options_script.cpp in each AI2C module) and the in-game
// settings (menu/settings.cpp). While the game's Options screen is open, the
// settings stand in for it, with the game's options on a tab of their own.
//
// The script thread opens a session when the screen shows (Opened), takes the
// changes made in the settings at each of its frames (TakeChange) and ends the
// session when the screen leaves (Left). The settings close with the screen's
// A (Accept), which the input gate presses until the screen leaves: the game
// then saves its options and returns to its menu as with its own screen.
//
// The executable keeps the one Session (game_options.cpp); the script modules
// are libraries of their own and find it by name (kSharedSymbol). It holds
// plain data only: each module runs its own copy of this code.
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>

namespace narutorise::game_options {

// The game's Option object, by fni_Option_GetValue's index. The volumes go
// from 0 to 10, the brightness from 6 to 26, the others are 0 or 1.
enum Option : int {
  kMusicVolume, kVoiceVolume, kEffectsVolume, kBrightness, kInvertX, kInvertY, kVibration, kSubtitles,
  kJapaneseVoices, kCount
};
struct Values {
  std::array<int, kCount> option{};
  // The screen offers them in the title menus, with their content installed.
  bool japanese_voices = false;
};

class Session {
 public:
  static constexpr int kNoChange = -1;
  // XInput's A, pressed for the screen while it accepts.
  static constexpr uint16_t kButtonA = 0x1000;
  // A held longer than this is let go, should the screen never leave.
  static constexpr int64_t kAcceptLimitMs = 3000;

  Session() {
    for (auto& change : changes_) change = kNoChange;
  }

  // The app: the settings stand in for the screen, and opened runs on the
  // script thread when it shows.
  void Enable(void (*opened)(const Values&)) {
    opened_ = opened;
    enabled_ = true;
  }
  // The settings: a changed option, and their closing.
  void Set(Option option, int value) {
    if (open_.load()) changes_[size_t(option)] = value;
  }
  void Accept() {
    if (open_.load() && !accepting_.exchange(true)) accepted_at_ = NowMs();
  }

  // The script hook.
  bool Enabled() const { return enabled_.load(); }
  bool Open() const { return open_.load(); }
  void Opened(const Values& values) {
    for (auto& change : changes_) change = kNoChange;
    accepting_ = false;
    open_ = true;
    if (opened_) opened_(values);
  }
  // The option's latest value from the settings since the last take, or kNoChange.
  int TakeChange(Option option) { return changes_[size_t(option)].exchange(kNoChange); }
  void Left() {
    open_ = false;
    accepting_ = false;
  }

  // The input gate: the buttons pressed for the screen, over the player's.
  uint16_t PressedButtons() {
    if (!accepting_.load()) return 0;
    if (NowMs() - accepted_at_.load() > kAcceptLimitMs) {
      accepting_ = false;
      return 0;
    }
    return kButtonA;
  }

 private:
  static int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
  std::atomic<bool> enabled_{false}, open_{false}, accepting_{false};
  std::atomic<int64_t> accepted_at_{0};
  std::array<std::atomic<int>, kCount> changes_;
  void (*opened_)(const Values&) = nullptr;
};

// The executable's exported function returning its Session.
inline constexpr char kSharedSymbol[] = "narutorise_game_options";

// In the executable (game_options.cpp): its Session, and the app's listener
// for the screen showing (on the script thread).
Session& Shared();
void Enable(std::function<void(const Values&)> opened);

}  // namespace narutorise::game_options
