#include "controls/runtime.h"
#include "menu/input_gate.h"
#include <rex/input/mnk/mnk_input_driver.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <chrono>
#include <optional>
#include <rex/ui/keybinds.h>
#include <rex/cvar.h>
namespace narutorise::controls {
using namespace rex::input;
using rex::X_RESULT;
using rex::X_STATUS;
namespace {
constexpr uint16_t kButtons[] = {0x1000, 0x2000, 0x4000, 0x8000, 0x100, 0x200,
                                 0,      0,      0x40,   0x80,   0x10,  0x20};
double Deadzone(double value, double deadzone) {
  return std::clamp((value - deadzone) / (1 - deadzone), 0.0, 1.0);
}
RawState Raw(const X_INPUT_GAMEPAD& p) {
  RawState raw;
  raw.buttons = uint16_t(p.buttons);
  const int axes[] = {int16_t(p.thumb_lx), int16_t(p.thumb_ly), int16_t(p.thumb_rx),
                      int16_t(p.thumb_ry)};
  for (int i = 0; i < 4; ++i)
    raw.axes[i] = axes[i] / (axes[i] < 0 ? 32768.0 : 32767.0);
  raw.axes[4] = p.left_trigger / 255.0;
  raw.axes[5] = p.right_trigger / 255.0;
  return raw;
}
X_INPUT_GAMEPAD Pad(const RawState& r) {
  X_INPUT_GAMEPAD out{};
  out.buttons = r.buttons;
  out.left_trigger = uint8_t(std::lround(std::clamp(r.axes[4], 0.0, 1.0) * 255));
  out.right_trigger = uint8_t(std::lround(std::clamp(r.axes[5], 0.0, 1.0) * 255));
  const auto axis = [](double v) {
    return int16_t(std::lround(std::clamp(v, -1.0, 1.0) * (v < 0 ? 32768 : 32767)));
  };
  out.thumb_lx = axis(r.axes[0]);
  out.thumb_ly = axis(r.axes[1]);
  out.thumb_rx = axis(r.axes[2]);
  out.thumb_ry = axis(r.axes[3]);
  return out;
}
class Assignment final : public DeviceAssignment {
 public:
  void OnDevicesChanged(const std::vector<DeviceInfo>& devices) override {
    std::lock_guard lock(mutex_);
    devices_ = devices;
    fallback_.OnDevicesChanged(devices);
  }
  void DevicesForUser(uint32_t user, std::vector<DeviceId>& out) const override {
    out.clear();
    if (user >= kPorts)
      return;
    auto config = Current();
    std::lock_guard lock(mutex_);
    if (!config->version) {
      fallback_.DevicesForUser(user, out);
      return;
    }
    const auto& key = config->ports[user].device;
    if (key.empty())
      return;
    if (key == kAutomaticDevice) {
      std::vector<std::string> pads;
      for (const auto& d : devices_) if (!d.synthetic) pads.push_back(d.key);
      const auto pad = AutomaticPad(*config, pads);
      for (const auto& d : devices_)
        if ((!pad.empty() && d.key == pad) ||
            (d.key == "keyboard" && AvailableForAutomatic(*config, d.key)))
          out.push_back(d.id);
      return;
    }
    for (const auto& d : devices_)
      if (d.key == key)
        out.push_back(d.id);
    if (out.size() != 1)
      out.clear();  // Never choose arbitrarily between duplicate serials.
  }

 private:
  mutable std::mutex mutex_;
  std::vector<DeviceInfo> devices_;
  SlotAssignment fallback_;
};
class Driver final : public InputDriver {
 public:
  explicit Driver(std::unique_ptr<InputDriver> driver)
      : InputDriver(nullptr, 0), driver_(std::move(driver)) {
    driver_->set_is_active_callback([this] { return is_active(); });
  }
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void OnWindowAvailable(rex::ui::Window* window) override { driver_->OnWindowAvailable(window); }
  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    std::vector<DeviceInfo> devices;
    driver_->EnumerateDevices(devices);
    std::lock_guard lock(mutex_);
    keys_.clear();
    for (const auto& d : devices) {
      keys_[d.id] = d.key;
      out.push_back(d);
    }
    std::erase_if(states_, [&](const auto& state) { return !keys_.contains(state.first); });
  }
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags, X_INPUT_CAPABILITIES* out) override {
    auto result = driver_->GetDeviceCapabilities(id, flags, out);
    if (result != X_ERROR_SUCCESS || !out)
      return result;
    auto config = Current();
    std::lock_guard lock(mutex_);
    const Port* port = FindPort(*config, id);
    if (config->version && !port)
      return X_ERROR_DEVICE_NOT_CONNECTED;
    if (!port)
      return result;
    const auto native = out->gamepad;
    const bool keyboard = keys_[id] == "keyboard";
    std::array<bool, kKeyActionCount> supported{};
    const int axes[] = {int16_t(native.thumb_lx), int16_t(native.thumb_ly),
                        int16_t(native.thumb_rx), int16_t(native.thumb_ry),
                        native.left_trigger,      native.right_trigger};
    for (size_t i = 0; i < supported.size(); ++i) {
      if (keyboard) {
        for (auto key : Alternatives(port->keys[i])) {
          if (key.starts_with("Mouse")) {
            supported[i] |= port->mouse && (key == "MouseUp" || key == "MouseDown" ||
                                            key == "MouseLeft" || key == "MouseRight");
            continue;
          }
          while (key.find('+') != std::string::npos)
            key = key.substr(key.find('+') + 1);
          supported[i] |= rex::ui::ParseVirtualKey(key) != rex::ui::VirtualKey::kNone;
        }
      } else {
        const auto source = port->pad[i];
        supported[i] = source.kind == SourceKind::Button
                           ? (uint16_t(native.buttons) & source.code) != 0
                           : source.kind == SourceKind::Axis && source.code >= 0 &&
                                 source.code < 6 && axes[source.code] != 0;
      }
    }
    out->gamepad = {};
    uint16_t buttons = 0;
    for (size_t i = 0; i < 12; ++i)
      if (supported[i])
        buttons |= kButtons[i];
    for (int i = 0; i < 4; ++i)
      if (supported[20 + i])
        buttons |= uint16_t(1u << i);
    if (!keyboard)
      buttons |= uint16_t(native.buttons) & 0x400;
    else if (!rex::cvar::GetFlagByName("keybind_guide").empty())
      buttons |= 0x400;
    out->gamepad.buttons = buttons;
    out->gamepad.left_trigger = supported[6] ? 255 : 0;
    out->gamepad.right_trigger = supported[7] ? 255 : 0;
    out->gamepad.thumb_lx = (supported[14] || supported[15]) ? 32767 : 0;
    out->gamepad.thumb_ly = (supported[12] || supported[13]) ? 32767 : 0;
    out->gamepad.thumb_rx = (supported[18] || supported[19]) ? 32767 : 0;
    out->gamepad.thumb_ry = (supported[16] || supported[17]) ? 32767 : 0;
    out->flags = (buttons & 0x3f) == 0x3f ? (uint16_t(out->flags) & ~X_INPUT_CAPS_NO_NAVIGATION)
                                          : (uint16_t(out->flags) | X_INPUT_CAPS_NO_NAVIGATION);
    if (keyboard) {
      out->vibration = {};
      out->flags = uint16_t(out->flags) & ~X_INPUT_CAPS_FFB_SUPPORTED;
    }
    return result;
  }
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out) override {
    X_INPUT_STATE state{};
    auto result = driver_->GetDeviceState(id, &state);
    if (result != X_ERROR_SUCCESS)
      return result;
    auto config = Current();
    std::lock_guard lock(mutex_);
    const Port* port = FindPort(*config, id);
    if (config->version && !port)
      return X_ERROR_DEVICE_NOT_CONNECTED;
    if (port && keys_[id] != "keyboard")
      state.gamepad = Pad(Transform(Raw(state.gamepad), *port));
    auto& last = states_[id];
    const size_t owner = port ? size_t(port - config->ports.data()) : kPorts;
    if (last.owner != owner || (port && (!last.profile || *last.profile != *port))) {
      last.strokes.clear();
      last.stroke_fields = 0;
      last.repeat_key = -1;
      ++last.packet;
      last.owner = owner;
      if (port)
        last.profile = *port;
      else
        last.profile.reset();
    }
    if (!last.packet || std::memcmp(&last.pad, &state.gamepad, sizeof(last.pad))) {
      ++last.packet;
      last.pad = state.gamepad;
    }
    state.packet_number = last.packet;
    if (out)
      *out = state;
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, X_INPUT_KEYSTROKE* out) override {
    X_INPUT_STATE state{};
    auto result = GetDeviceState(id, &state);
    if (result != X_ERROR_SUCCESS)
      return result;
    std::lock_guard lock(mutex_);
    auto& last = states_[id];
    const auto after = Fields(state.gamepad), before = last.stroke_fields;
    const auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < kVks.size(); ++i)
      if ((before ^ after) & (1ull << i)) {
        X_INPUT_KEYSTROKE stroke{};
        stroke.virtual_key = kVks[i];
        const bool down = (after & (1ull << i)) != 0;
        stroke.flags = down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
        if (last.strokes.size() < 128)
          last.strokes.push_back(stroke);
        if (down) {
          last.repeat_key = int(i);
          last.repeat_at = now + std::chrono::milliseconds(400);
        }
      }
    last.stroke_fields = after;
    if (!after)
      last.repeat_key = -1;
    else if (last.repeat_key >= 0 && !(after & (1ull << last.repeat_key)))
      last.repeat_key = -1;
    if (last.repeat_key >= 0 && now >= last.repeat_at) {
      X_INPUT_KEYSTROKE repeat{};
      repeat.virtual_key = kVks[size_t(last.repeat_key)];
      repeat.flags = X_INPUT_KEYSTROKE_KEYDOWN | X_INPUT_KEYSTROKE_REPEAT;
      if (last.strokes.size() < 128)
        last.strokes.push_back(repeat);
      last.repeat_at = now + std::chrono::milliseconds(100);
    }
    if (last.strokes.empty())
      return X_ERROR_EMPTY;
    if (out)
      *out = last.strokes.front();
    last.strokes.pop_front();
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* value) override {
    auto config = Current();
    X_INPUT_VIBRATION scaled = *value;
    {
      std::lock_guard lock(mutex_);
      const Port* port = FindPort(*config, id);
      if (config->version && !port)
        return X_ERROR_DEVICE_NOT_CONNECTED;
      if (narutorise::menu::input_blocked.load())
        return X_ERROR_SUCCESS;
      if (port) {
        scaled.left_motor_speed =
            uint16_t(std::lround(uint16_t(value->left_motor_speed) * port->rumble[0]));
        scaled.right_motor_speed =
            uint16_t(std::lround(uint16_t(value->right_motor_speed) * port->rumble[1]));
      }
    }
    return driver_->SetDeviceVibration(id, &scaled);
  }

 private:
  const Port* FindPort(const Config& config, DeviceId id) const {
    auto key = keys_.find(id);
    if (key == keys_.end())
      return nullptr;
    for (const auto& p : config.ports)
      if (!p.device.empty() && p.device == key->second)
        return &p;
    if (AvailableForAutomatic(config, key->second))
      for (const auto& p : config.ports)
        if (p.device == kAutomaticDevice) return &p;
    return nullptr;
  }
  // Xbox keystrokes, generated from the same transformed state as GetState.
  static constexpr std::array<uint16_t, 32> kVks{
      0x5800, 0x5801, 0x5802, 0x5803, 0x5805, 0x5804, 0x5806, 0x5807, 0x5810, 0x5811, 0x5812,
      0x5813, 0x5816, 0x5817, 0x5820, 0x5821, 0x5822, 0x5823, 0x5824, 0x5825, 0x5826, 0x5827,
      0x5830, 0x5831, 0x5832, 0x5833, 0x5834, 0x5835, 0x5836, 0x5837, 0x5814, 0x5815};
  static uint64_t Fields(const X_INPUT_GAMEPAD& pad) {
    const auto r = Raw(pad);
    uint64_t out = 0;
    // Canonical first eight, dpad, stick directions/clicks, Start/Back.
    constexpr uint16_t bits[] = {0x1000, 0x2000, 0x4000, 0x8000, 0x100, 0x200};
    for (int i = 0; i < 6; ++i)
      if (r.buttons & bits[i])
        out |= 1ull << i;
    if (r.axes[4] > .12)
      out |= 1ull << 6;
    if (r.axes[5] > .12)
      out |= 1ull << 7;
    for (int i = 0; i < 4; ++i)
      if (r.buttons & (1u << i))
        out |= 1ull << (8 + i);
    if (r.buttons & 0x40)
      out |= 1ull << 12;
    if (r.buttons & 0x80)
      out |= 1ull << 13;
    for (int stick = 0; stick < 2; ++stick) {
      const double x = r.axes[stick * 2], y = r.axes[stick * 2 + 1];
      const bool up = y > .6, down = y < -.6, left = x < -.6, right = x > .6;
      const int direction = up      ? (left    ? 4
                                       : right ? 5
                                               : 0)
                            : down  ? (right  ? 6
                                       : left ? 7
                                              : 1)
                            : right ? 2
                            : left  ? 3
                                    : -1;
      if (direction >= 0)
        out |= 1ull << (14 + stick * 8 + direction);
    }
    if (r.buttons & 0x10)
      out |= 1ull << 30;
    if (r.buttons & 0x20)
      out |= 1ull << 31;
    return out;
  }
  std::unique_ptr<InputDriver> driver_;
  std::mutex mutex_;
  std::map<DeviceId, std::string> keys_;
  struct Last {
    X_INPUT_GAMEPAD pad{};
    uint32_t packet = 0;
    std::deque<X_INPUT_KEYSTROKE> strokes;
    std::optional<Port> profile;
    size_t owner = kPorts;
    uint64_t stroke_fields = 0;
    int repeat_key = -1;
    std::chrono::steady_clock::time_point repeat_at{};
  };
  std::map<DeviceId, Last> states_;
};
}  // namespace
RawState Transform(const RawState& raw, const Port& port) {
  std::array<double, kKeyActionCount> values{};
  for (size_t i = 0; i < values.size(); ++i) {
    auto s = port.pad[i];
    if (s.kind == SourceKind::Button)
      values[i] = (raw.buttons & s.code) ? 1 : 0;
    else if (s.kind == SourceKind::Axis && s.code >= 0 && s.code < 6)
      values[i] = std::clamp(raw.axes[s.code] * s.sign, 0.0, 1.0);
  }
  RawState out;
  out.buttons = raw.buttons & 0x400;
  for (size_t i = 0; i < 12; ++i)
    if (values[i] > .5)
      out.buttons |= kButtons[i];
  for (int i = 0; i < 4; ++i)
    if (values[20 + i] > .5)
      out.buttons |= uint16_t(1u << i);
  out.axes = {values[15] - values[14],
              values[12] - values[13],
              values[19] - values[18],
              values[16] - values[17],
              values[6],
              values[7]};
  if (port.deadzones) {
    for (int stick = 0; stick < 2; ++stick) {
      double& x = out.axes[stick * 2];
      double& y = out.axes[stick * 2 + 1];
      const double length = std::hypot(x, y),
                   amount = Deadzone(std::min(length, 1.0), port.deadzone[stick]);
      if (length > 0) {
        x = x / length * amount;
        y = y / length * amount;
      }
    }
    out.axes[4] = Deadzone(out.axes[4], port.deadzone[2]);
    out.axes[5] = Deadzone(out.axes[5], port.deadzone[3]);
  }
  return out;
}
std::unique_ptr<InputDriver> ConfigureInput(std::unique_ptr<InputDriver> driver) {
  return std::make_unique<Driver>(std::move(driver));
}
std::unique_ptr<DeviceAssignment> ConfiguredAssignment() {
  return std::make_unique<Assignment>();
}
Config Migrate(Config config) {
  if (config.version)
    return config;
  for (auto& p : config.ports) {
    p.device.clear();
    p.device_name.clear();
  }
  Assign(config, 0, kAutomaticDevice, {});
  config.version = 1;
  return config;
}
}  // namespace narutorise::controls
