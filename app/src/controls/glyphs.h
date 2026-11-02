// The buttons a prompt shows: the family of the controller player one uses, or
// the keyboard and mouse, as the last device that player touched.
#pragma once
#include "controls/config.h"
#include <SDL3/SDL_gamepad.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace narutorise::controls {
struct Inventory;

// The families of app/ui/images/prompts.png (tools/input-prompts/build-atlas.py).
enum class Glyphs : uint8_t { Xbox, PlayStation, Nintendo, SteamDeck, Keyboard };
// The atlas prefix of a family: "xbox", "ps", "switch", "deck" or "key".
const char* GlyphPrefix(Glyphs glyphs);

// The settings' choice: "auto" keeps each controller's own family; the others
// show that family for every controller (a virtual pad hides the real one).
inline constexpr const char* kGlyphPreferences[] = {"auto", "xbox", "playstation", "nintendo", "steamdeck"};
// The family SDL reports. The Steam Deck reports a generic pad.
Glyphs PadGlyphs(SDL_GamepadType type, uint16_t vendor, uint16_t product);
// detected, or the family the preference names.
Glyphs ForPad(Glyphs detected, std::string_view preference);

// Which of player one's devices was used last (UI thread). Player one is the
// port whose prompts the game draws; the keyboard counts only while it plays
// for that port.
class ActiveInput {
 public:
  // After each hardware poll: player one's pad leaving neutral takes over.
  // Stick drift below half a deflection does not.
  void ObservePads(const Inventory& inventory, const Config& config);
  // A key, a mouse button or the wheel.
  void ObserveKeyboard(const Config& config);
  // Mouse motion takes over past a distance, so a nudged mouse does not.
  void ObserveMouseMotion(int dx, int dy, const Config& config,
                          std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
  Glyphs Current(const Inventory& inventory, const Config& config, std::string_view preference) const;

 private:
  bool keyboard_ = false;  // the keyboard was used after the pad
  std::string engaged_;    // the pad held off neutral at the last poll
  int motion_ = 0;
  std::chrono::steady_clock::time_point motion_start_{};
};

// The preference of the settings, for every thread.
void SetGlyphPreference(std::string preference);
std::string GlyphPreference();

// Player one's glyphs for the threads that draw the game's prompts: written by
// the UI thread after each poll, with a revision that changes with them and
// with each published profile (InvalidateGlyphs).
void PublishGlyphs(Glyphs glyphs);
Glyphs PublishedGlyphs();
uint64_t GlyphsRevision();
void InvalidateGlyphs();

// The atlas sprite of a guest input as the port's profile maps it, in those
// glyphs: "xbox-east" for an A moved to B, "key-space" for A on the keyboard.
// Inputs: south, east, west, north (A, B, X, Y), lb, rb, lt, rt, ls, rs,
// start, back, and the whole dpad, lstick and rstick. Empty when unbound.
std::string GlyphSprite(std::string_view input, Glyphs glyphs, const Port& port);
}  // namespace narutorise::controls
