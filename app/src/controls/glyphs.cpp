#include "controls/glyphs.h"
#include "controls/service.h"
#include <cctype>
#include <cstdlib>
#include <mutex>

namespace narutorise::controls {
namespace {
// Half a deflection: drift stays well below it, a deliberate push passes it.
constexpr double kEngaged = .5;
// Mouse pixels within kMotionWindow that count as using the mouse.
constexpr int kMotionDistance = 24;
constexpr auto kMotionWindow = std::chrono::milliseconds(300);
constexpr uint16_t kValve = 0x28de, kSteamDeck = 0x1205;

std::mutex preference_mutex;
std::string preference = "auto";
std::atomic<uint8_t> published{uint8_t(Glyphs::Xbox)};
std::atomic<uint64_t> revision{1};

bool KeyboardPlaysFirst(const Config& config) { return KeyboardPort(config) == 0; }
}  // namespace

const char* GlyphPrefix(Glyphs glyphs) {
  switch (glyphs) {
    case Glyphs::PlayStation: return "ps";
    case Glyphs::PlayStation4: return "ps4";
    case Glyphs::Nintendo: return "switch";
    case Glyphs::SteamDeck: return "deck";
    case Glyphs::Keyboard: return "key";
    case Glyphs::Xbox: break;
  }
  return "xbox";
}

Glyphs PadGlyphs(SDL_GamepadType type, uint16_t vendor, uint16_t product) {
  if (vendor == kValve && product == kSteamDeck) return Glyphs::SteamDeck;
  switch (type) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4: return Glyphs::PlayStation4;
    case SDL_GAMEPAD_TYPE_PS5: return Glyphs::PlayStation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
    case SDL_GAMEPAD_TYPE_GAMECUBE: return Glyphs::Nintendo;
    default: return Glyphs::Xbox;
  }
}

Glyphs ForPad(Glyphs detected, std::string_view choice) {
  if (choice == "xbox") return Glyphs::Xbox;
  if (choice == "playstation") return Glyphs::PlayStation;
  if (choice == "playstation4") return Glyphs::PlayStation4;
  if (choice == "nintendo") return Glyphs::Nintendo;
  if (choice == "steamdeck") return Glyphs::SteamDeck;
  return detected == Glyphs::Keyboard ? Glyphs::Xbox : detected;
}

void ActiveInput::ObservePads(const Inventory& inventory, const Config& config) {
  const Device* pad = SelectedDevice(config, 0, inventory);
  if (!pad || pad->keyboard) {
    engaged_.clear();
    return;
  }
  const bool engaged = !pad->raw.Neutral(kEngaged);
  if (engaged && engaged_ != pad->key) keyboard_ = false;
  engaged_ = engaged ? pad->key : std::string();
}

void ActiveInput::ObserveKeyboard(const Config& config) {
  if (KeyboardPlaysFirst(config)) keyboard_ = true;
}

void ActiveInput::ObserveMouseMotion(int dx, int dy, const Config& config,
                                     std::chrono::steady_clock::time_point now) {
  if (now - motion_start_ > kMotionWindow) {
    motion_start_ = now;
    motion_ = 0;
  }
  motion_ += std::abs(dx) + std::abs(dy);
  if (motion_ >= kMotionDistance) ObserveKeyboard(config);
}

Glyphs ActiveInput::Current(const Inventory& inventory, const Config& config, std::string_view choice) const {
  const Device* device = SelectedDevice(config, 0, inventory);
  const bool keyboard = KeyboardPlaysFirst(config);
  if (keyboard && (keyboard_ || !device || device->keyboard)) return Glyphs::Keyboard;
  return ForPad(device && !device->keyboard ? device->glyphs : Glyphs::Xbox, choice);
}

void SetGlyphPreference(std::string choice) {
  std::lock_guard lock(preference_mutex);
  preference = std::move(choice);
}

std::string GlyphPreference() {
  std::lock_guard lock(preference_mutex);
  return preference;
}

void PublishGlyphs(Glyphs glyphs) {
  if (published.exchange(uint8_t(glyphs), std::memory_order_acq_rel) != uint8_t(glyphs))
    revision.fetch_add(1, std::memory_order_acq_rel);
}

Glyphs PublishedGlyphs() { return Glyphs(published.load(std::memory_order_acquire)); }

uint64_t GlyphsRevision() { return revision.load(std::memory_order_acquire); }

void InvalidateGlyphs() { revision.fetch_add(1, std::memory_order_acq_rel); }

namespace {
// The guest inputs by their profile action (key_bindings.h order).
struct GuestInput {
  std::string_view name;
  size_t action;
};
constexpr GuestInput kInputs[] = {
    {"south", 0}, {"east", 1}, {"west", 2}, {"north", 3}, {"lb", 4}, {"rb", 5}, {"lt", 6}, {"rt", 7},
    {"ls", 8}, {"rs", 9}, {"start", 10}, {"back", 11},
    // A whole stick or pad shows the source of its up direction.
    {"lstick", kLeftStickFirst}, {"rstick", kRightStickFirst}, {"dpad", kDpadFirst},
};
std::string Lower(std::string_view text) {
  std::string out(text);
  for (auto& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}
// The physical position a pad source stands for.
std::string PadPosition(const Source& source, bool whole) {
  if (source.kind == SourceKind::Button) {
    constexpr std::pair<int, const char*> buttons[] = {
        {0x1000, "south"}, {0x2000, "east"}, {0x4000, "west"}, {0x8000, "north"}, {0x100, "lb"},
        {0x200, "rb"}, {0x40, "ls"}, {0x80, "rs"}, {0x10, "start"}, {0x20, "back"},
        {1, "dpad-up"}, {2, "dpad-down"}, {4, "dpad-left"}, {8, "dpad-right"}};
    for (const auto& [bit, name] : buttons)
      if (source.code == bit) return whole && bit < 0x10 ? "dpad" : name;
    return {};
  }
  if (source.kind != SourceKind::Axis) return {};
  if (source.code == 4) return "lt";
  if (source.code == 5) return "rt";
  if (source.code < 0 || source.code > 3) return {};
  const std::string stick = source.code < 2 ? "lstick" : "rstick";
  if (whole) return stick;
  const bool vertical = source.code & 1;
  return stick + (vertical ? (source.sign > 0 ? "-up" : "-down") : (source.sign > 0 ? "-right" : "-left"));
}
}  // namespace

std::string GlyphSprite(std::string_view input, Glyphs glyphs, const Port& port) {
  const GuestInput* found = nullptr;
  for (const auto& candidate : kInputs)
    if (candidate.name == input) found = &candidate;
  if (!found) return {};
  const bool whole = found->action >= kLeftStickFirst;
  const std::string prefix = std::string(GlyphPrefix(glyphs)) + "-";
  if (glyphs != Glyphs::Keyboard) {
    const auto position = PadPosition(port.pad[found->action], whole);
    return position.empty() ? std::string() : prefix + position;
  }
  // The mouse turns the camera: the right stick shows it.
  const auto& keys = port.keys[found->action];
  if (found->action == kRightStickFirst && port.mouse && keys.find("Mouse") != std::string::npos)
    return prefix + "mouse";
  for (const auto& key : Alternatives(keys)) {
    if (key.empty()) continue;
    if (key == "MouseUp" || key == "MouseDown") return prefix + "mousevertical";
    if (key == "MouseLeft" || key == "MouseRight") return prefix + "mousehorizontal";
    return prefix + Lower(key);
  }
  return {};
}
}  // namespace narutorise::controls
