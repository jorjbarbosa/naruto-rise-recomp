// Persistent input profiles. Device keys identify hardware, never SDL handles.
#pragma once
#include "../menu/key_bindings.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace narutorise::controls {
inline constexpr size_t kPorts = 4;
inline constexpr const char* kAutomaticDevice = "auto";
enum class SourceKind { None, Button, Axis };
struct Source {
  SourceKind kind = SourceKind::None;
  int code = 0, sign = 1;
  bool operator==(const Source&) const = default;
};
// Codes are Xbox button bits and LX, LY, RX, RY, LT, RT. Y is positive up.
inline std::array<Source, kKeyActionCount> DefaultPad() {
  std::array<Source, kKeyActionCount> out{};
  constexpr int buttons[] = {0x1000, 0x2000, 0x4000, 0x8000, 0x100, 0x200,
                             0,      0,      0x40,   0x80,   0x10,  0x20};
  for (int i = 0; i < 12; ++i)
    out[i] = {SourceKind::Button, buttons[i]};
  out[6] = {SourceKind::Axis, 4};
  out[7] = {SourceKind::Axis, 5};
  for (int stick = 0; stick < 2; ++stick) {
    const int at = 12 + stick * 4, x = stick * 2, y = x + 1;
    out[at] = {SourceKind::Axis, y, 1};
    out[at + 1] = {SourceKind::Axis, y, -1};
    out[at + 2] = {SourceKind::Axis, x, -1};
    out[at + 3] = {SourceKind::Axis, x, 1};
  }
  for (int i = 0; i < 4; ++i)
    out[20 + i] = {SourceKind::Button, 1 << i};
  return out;
}
inline KeyBinds DefaultKeys() {
  auto out = DefaultKeyBinds();
  constexpr const char* mouse[] = {"MouseUp", "MouseDown", "MouseLeft", "MouseRight"};
  for (int i = 0; i < 4; ++i)
    out[16 + i] += std::string(",") + mouse[i];
  return out;
}
struct Port {
  std::string device;       // empty: None; "auto"; "keyboard"; backend-qualified identity
  std::string device_name;  // last known real name; retained on disconnect
  std::array<Source, kKeyActionCount> pad = DefaultPad();
  KeyBinds keys = DefaultKeys();
  bool deadzones = true, mouse = true;
  std::array<double, 4> deadzone{.15, .15, .05, .05};  // LS, RS, LT, RT
  double sensitivity = 1.0;
  std::array<double, 2> rumble{1.0, 1.0};
  bool operator==(const Port&) const = default;
};
struct Config {
  int version = 0;  // legacy; migrated to automatic player one before the first save
  std::array<Port, kPorts> ports{};
  bool operator==(const Config&) const = default;
};
std::vector<std::string> Alternatives(const std::string& text);
std::string Join(const std::vector<std::string>& values);
void BindKey(Port& port, size_t action, size_t slot, const std::string& key);
// The SDK removes standalone modifiers from its live chord mask. Check the
// proposed bank in either editing order, including the global Guide binding.
bool ModifierConflict(const KeyBinds& bindings, const std::string& key, const std::string& guide = {});
void Restore(Port& port);
void Assign(Config& config, size_t port, const std::string& device, const std::string& name);
bool AvailableForAutomatic(const Config& config, const std::string& device);
// A stable choice shared by runtime and menu inventories, independent of their
// enumeration order. Explicit assignments and ambiguous identities are excluded.
std::string AutomaticPad(const Config& config, const std::vector<std::string>& keys);
bool Valid(const Config& config);
std::string Encode(Source source);
Source Decode(const std::string& text);
}  // namespace narutorise::controls
