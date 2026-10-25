#include "controls/runtime.h"
#include "menu/input_gate.h"
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <iostream>
#include <cmath>
#include <stdexcept>
using namespace narutorise::controls;
using namespace rex::input;
using rex::X_RESULT;
using rex::X_STATUS;
void Check(bool condition, const char* why) {
  if (!condition)
    throw std::runtime_error(why);
}
class Physical final : public InputDriver {
 public:
  Physical() : InputDriver(nullptr, 0) {}
  X_INPUT_STATE state{};
  X_INPUT_VIBRATION rumble{};
  bool present = true;
  std::string key = "fixture:physical";
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    if (present) {
      DeviceInfo d;
      d.id = DeviceId(12);
      d.key = key;
      d.name = "Test fixture";
      out.push_back(d);
    }
  }
  X_RESULT GetDeviceState(DeviceId, X_INPUT_STATE* out) override {
    *out = state;
    return present ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT GetDeviceCapabilities(DeviceId, uint32_t, X_INPUT_CAPABILITIES* out) override {
    *out = {};
    out->type = 1;
    out->gamepad.buttons = 0xffff;
    out->gamepad.left_trigger = out->gamepad.right_trigger = 255;
    out->gamepad.thumb_lx = out->gamepad.thumb_ly = out->gamepad.thumb_rx = out->gamepad.thumb_ry =
        32767;
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetDeviceVibration(DeviceId, X_INPUT_VIBRATION* value) override {
    rumble = *value;
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetDeviceKeystroke(DeviceId, uint32_t, X_INPUT_KEYSTROKE*) override {
    return X_ERROR_EMPTY;
  }
};
int main() {
  try {
    Config config;
    config.version = 1;
    Assign(config, 0, "fixture:physical", "Test fixture");
    config.ports[0].pad[0] = {SourceKind::Button, 0x2000};
    config.ports[0].pad[1] = {};
    config.ports[0].rumble = {.5, .25};
    Publish(config);
    InputSystem input(nullptr);
    auto physical = std::make_unique<Physical>();
    auto* raw = physical.get();
    input.AddDriver(ConfigureInput(std::move(physical)));
    input.SetDeviceAssignment(ConfiguredAssignment());
    raw->state.gamepad.buttons = 0x2000;
    X_INPUT_STATE state{};
    Check(input.GetState(0, &state) == X_ERROR_SUCCESS && uint16_t(state.gamepad.buttons) == 0x1000,
          "remap B to A");
    Check(input.GetState(1, &state) == X_ERROR_DEVICE_NOT_CONNECTED, "None really disconnected");
    X_INPUT_KEYSTROKE stroke{};
    Check(input.GetKeystroke(0, 0, &stroke) == X_ERROR_SUCCESS &&
              uint16_t(stroke.virtual_key) == 0x5800,
          "keystroke uses remapped A");
    X_INPUT_CAPABILITIES mapped_caps{};
    Check(input.GetCapabilities(0, 0, &mapped_caps) == X_ERROR_SUCCESS &&
              (uint16_t(mapped_caps.gamepad.buttons) & 0x1000) &&
              !(uint16_t(mapped_caps.gamepad.buttons) & 0x2000),
          "capabilities follow mapping");
    raw->state.gamepad.buttons = 0;
    input.GetState(0, &state);
    input.GetKeystroke(0, 0, &stroke);
    raw->state.gamepad.buttons = 0x8000;
    input.GetState(0, &state);
    raw->state.gamepad.buttons = 0;
    input.GetState(0, &state);
    Check(input.GetKeystroke(0, 0, &stroke) == X_ERROR_EMPTY,
          "state-only history must not become stale keystrokes");
    raw->state.gamepad.buttons = 0x2000;
    X_INPUT_VIBRATION vibration{};
    vibration.left_motor_speed = 40000;
    vibration.right_motor_speed = 20000;
    Check(input.SetState(0, &vibration) == X_ERROR_SUCCESS &&
              uint16_t(raw->rumble.left_motor_speed) == 20000 &&
              uint16_t(raw->rumble.right_motor_speed) == 5000,
          "rumble scale");
    auto previous = config;
    Assign(config, 2, "fixture:physical", "Test fixture");
    Publish(config);
    Check(input.GetState(0, &state) == X_ERROR_DEVICE_NOT_CONNECTED &&
              input.GetState(2, &state) == X_ERROR_SUCCESS,
          "live transfer");
    Check(config.ports[0].pad == previous.ports[0].pad, "transfer preserves mappings");
    Check(input.GetKeystroke(2, 0, &stroke) == X_ERROR_SUCCESS &&
              uint16_t(stroke.virtual_key) == 0x5801 && stroke.user_index == 2,
          "transferred owner gets fresh correct keystroke");
    raw->key = "fixture:updated";
    config.ports[2].device = raw->key;
    Publish(config);
    Check(input.GetState(2, &state) == X_ERROR_SUCCESS,
          "identity metadata refresh without changing handle");
    raw->present = false;
    Check(input.GetState(2, &state) == X_ERROR_DEVICE_NOT_CONNECTED, "disconnect");
    Port p;
    RawState sample;
    sample.axes = {.1, 0, 0, 0, .04, 1};
    auto result = Transform(sample, p);
    Check(result.axes[0] == 0 && result.axes[4] == 0 && result.axes[5] == 1, "deadzone at rest");
    sample.axes[0] = .575;
    result = Transform(sample, p);
    Check(std::abs(result.axes[0] - .5) < .001, "radial deadzone rescale");
    p.deadzones = false;
    Check(Transform(sample, p).axes == sample.axes, "deadzone disabled");
    p.pad[15] = {};
    Check(Transform(sample, p).axes[0] == 0, "removed axis");
    Assign(config, 1, "keyboard", "Keyboard");
    config.ports[1].keys[0] = "G";
    config.ports[1].sensitivity = 2.5;
    Publish(config);
    auto profile = mnk::GetProfile();
    Check(profile && profile->user == 1 && profile->sensitivity == 2.5,
          "effective keyboard profile");
    mnk::MnkInputDriver keyboard(nullptr, 0);
    keyboard.Setup();
    rex::ui::KeyEvent down(nullptr, rex::ui::VirtualKey::kG, 1, false, false, false, false, false);
    keyboard.OnKeyDown(down);
    std::vector<DeviceInfo> devices;
    keyboard.EnumerateDevices(devices);
    Check(devices.size() == 1, "keyboard enabled");
    keyboard.GetDeviceState(devices[0].id, &state);
    Check(uint16_t(state.gamepad.buttons) & 0x1000, "keyboard bindings use owner profile");
    rex::ui::KeyEvent up(nullptr, rex::ui::VirtualKey::kG, 1, true, false, false, false, false);
    keyboard.OnKeyUp(up);
    keyboard.GetDeviceState(devices[0].id, &state);
    Check(!uint16_t(state.gamepad.buttons), "keyboard release");
    float dx = 1, dy = 1;
    mnk::TakeMouseCameraDelta(dx, dy);
    Check(dx == 0 && dy == 0, "keyboard on J2 cannot drive direct J1 camera");
    X_INPUT_CAPABILITIES caps{};
    keyboard.GetDeviceCapabilities(devices[0].id, 0, &caps);
    Check(!uint16_t(caps.vibration.left_motor_speed), "keyboard has no motors");
    mnk::NoteMouseCameraHook();
    rex::ui::MouseEvent move(nullptr, rex::ui::MouseEvent::Button::kNone, 10, 0);
    keyboard.OnMouseMove(move);
    keyboard.GetDeviceState(devices[0].id, &state);
    Check(int16_t(state.gamepad.thumb_rx) > 0,
          "J2 uses ordinary right-stick mouse path even while J1 hook runs");
    config.ports[1].keys[19] = "Right";
    Publish(config);
    rex::ui::MouseEvent removed_motion(nullptr, rex::ui::MouseEvent::Button::kNone, 20, 0);
    keyboard.OnMouseMove(removed_motion);
    keyboard.GetDeviceState(devices[0].id, &state);
    Check(!int16_t(state.gamepad.thumb_rx), "removed mouse direction stays removed");
    config.ports[1].keys[15] = "D,MouseRight";
    Publish(config);
    rex::ui::MouseEvent remapped_motion(nullptr, rex::ui::MouseEvent::Button::kNone, 30, 0);
    keyboard.OnMouseMove(remapped_motion);
    keyboard.GetDeviceState(devices[0].id, &state);
    Check(int16_t(state.gamepad.thumb_lx) > 0 && !int16_t(state.gamepad.thumb_rx),
          "mouse direction can map to movement without leaking to camera");
    auto inventory = Inventory{};
    Device d;
    d.key = "fixture:physical";
    d.name = "Test fixture";
    inventory.devices.push_back(d);
    Config legacy;
    auto migrated = Migrate(legacy, inventory, true);
    Check(migrated.ports[0].device == "keyboard" && migrated.ports[1].device == d.key,
          "approved legacy device priority");
    std::cout << "PASS: real InputSystem routing, remap/keystroke, transfer, None, deadzones, "
                 "rumble scale, MnK owner, migration\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
