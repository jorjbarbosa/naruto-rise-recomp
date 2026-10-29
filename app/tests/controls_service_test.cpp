#include "controls/service.h"
#include "controls/runtime.h"
#include <rex/input/input_system.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/ui/window.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
class FixtureContext final : public rex::ui::WindowedAppContext {
 protected:
  void NotifyUILoopOfPendingFunctions() override {}
  void PlatformQuitFromUIThread() override {}
};
// The real SDL driver starts only after AttachWindow. This fixture supplies
// its UI-thread/lifetime contract without opening a desktop window.
class FixtureWindow final : public rex::ui::Window {
 public:
  explicit FixtureWindow(FixtureContext& context) : Window(context, "Input fixture", 64, 64) {}
  ~FixtureWindow() override { EnterDestructor(); }
 protected:
  bool OpenImpl() override { return true; }
  void RequestCloseImpl() override {
    WindowDestructionReceiver receiver(this);
    OnBeforeClose(receiver);
    if (!receiver.IsWindowDestroyed()) OnAfterClose();
  }
  std::unique_ptr<rex::ui::Surface> CreateSurfaceImpl(rex::ui::Surface::TypeFlags) override { return {}; }
  void RequestPaintImpl() override {}
};
// Exercise SDL's actual events/state conversion while keeping real hardware
// outside the fixture's assignment, regardless of what the host has attached.
class FixtureSDL final : public rex::input::InputDriver {
 public:
  FixtureSDL() : InputDriver(nullptr, 0), driver_(nullptr, 0) {}
  rex::X_STATUS Setup() override { return driver_.Setup(); }
  void OnWindowAvailable(rex::ui::Window* window) override { driver_.OnWindowAvailable(window); }
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    driver_.EnumerateDevices(out);
    std::erase_if(out, [](const auto& d) { return d.name != "Automatic input fixture"; });
  }
  rex::X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out) override {
    return driver_.GetDeviceState(id, out);
  }
  rex::X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t flags,
                                     rex::input::X_INPUT_CAPABILITIES* out) override {
    return driver_.GetDeviceCapabilities(id, flags, out);
  }
  rex::X_RESULT SetDeviceVibration(rex::input::DeviceId id, rex::input::X_INPUT_VIBRATION* value) override {
    return driver_.SetDeviceVibration(id, value);
  }
  rex::X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t flags,
                                  rex::input::X_INPUT_KEYSTROKE* out) override {
    return driver_.GetDeviceKeystroke(id, flags, out);
  }
 private:
  rex::input::sdl::SDLInputDriver driver_;
};
void AutomaticSDL(narutorise::controls::Service& service) {
  using namespace narutorise::controls;
  using namespace rex::input;
  using rex::X_STATUS;
  using rex::X_RESULT;
  const auto require = [](bool ok, const char* label) { if (!ok) throw std::runtime_error(label); };
  service.Initialize();
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  Publish(Migrate({}));
  FixtureContext context;
  FixtureWindow window(context);
  require(window.Open(), "fixture UI lifecycle");
  InputSystem input(nullptr);
  auto driver = std::make_unique<FixtureSDL>();
  auto* actual = driver.get();
  require(driver->Setup() == X_STATUS_SUCCESS, "SDL driver setup");
  auto wrapped = ConfigureInput(std::move(driver));
  wrapped->set_is_active_callback([] { return true; });
  input.AddDriver(std::move(wrapped));
  SDL_VirtualJoystickDesc desc; SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD; desc.name = "Automatic input fixture";
  desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT; desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
  // The menu can initialize SDL and discover the controller before runtime
  // creation. Its already-delivered ADDED event must not be required again.
  const auto before_window = SDL_AttachVirtualJoystick(&desc);
  auto* early = before_window ? SDL_OpenJoystick(before_window) : nullptr;
  require(early != nullptr, "preconnected controller attached");
  service.Poll("sdl", true);
  input.AttachWindow(&window);
  input.SetDeviceAssignment(ConfiguredAssignment());
  SDL_Event duplicate{}; duplicate.type = SDL_EVENT_GAMEPAD_ADDED; duplicate.gdevice.which = before_window;
  require(SDL_PushEvent(&duplicate), "duplicate connection event");
  X_INPUT_STATE state{};
  require(input.GetState(0, &state) == X_ERROR_SUCCESS, "controller detected by menu survives runtime creation");
  std::vector<DeviceInfo> connected; actual->EnumerateDevices(connected);
  require(connected.size() == 1, "inventory and SDL event do not duplicate a device");
  require(SDL_SetJoystickVirtualButton(early, SDL_GAMEPAD_BUTTON_SOUTH, true), "preconnected A");
  service.Poll("sdl", true);
  require(input.GetState(0, &state) == X_ERROR_SUCCESS && (uint16_t(state.gamepad.buttons) & 0x1000),
          "preconnected controller input reaches the guest");
  SDL_CloseJoystick(early); require(SDL_DetachVirtualJoystick(before_window), "detach preconnected controller");
  service.Poll("sdl", true);
  require(input.GetState(0, &state) == X_ERROR_DEVICE_NOT_CONNECTED, "no fixture initially");
  for (unsigned connection = 0; connection < 2; ++connection) {
    const auto id = SDL_AttachVirtualJoystick(&desc);
    auto* joystick = id ? SDL_OpenJoystick(id) : nullptr;
    require(joystick != nullptr, "virtual controller attached");
    service.Poll("sdl", true);
    Inventory fixture;
    for (const auto& d : service.Devices()->devices)
      if (d.keyboard || (d.pad && SDL_GetGamepadID(d.pad) == id)) fixture.devices.push_back(d);
    const auto* selected = SelectedDevice(*Current(), 0, fixture);
    require(selected && !selected->keyboard, "menu resolves the automatic controller");
    require(input.GetState(0, &state) == X_ERROR_SUCCESS, "late device reaches the runtime inventory");
    require(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, true), "set virtual A");
    service.Poll("sdl", true);
    const auto status = input.GetState(0, &state);
    if (status != X_ERROR_SUCCESS || !(uint16_t(state.gamepad.buttons) & 0x1000)) {
      std::vector<DeviceInfo> listed; actual->EnumerateDevices(listed);
      std::cerr << "SDL fixture status=" << status << " buttons=" << uint16_t(state.gamepad.buttons)
                << " devices=" << listed.size() << " menu buttons=" << selected->raw.buttons << '\n';
      for (const auto& device : listed) std::cerr << device.name << ": " << device.key << '\n';
    }
    require(status == X_ERROR_SUCCESS && (uint16_t(state.gamepad.buttons) & 0x1000),
            "the same controller feeds player one without configuration");
    X_INPUT_CAPABILITIES caps{};
    require(input.GetCapabilities(0, 0, &caps) == X_ERROR_SUCCESS, "automatic controller capabilities");
    SDL_CloseJoystick(joystick);
    require(SDL_DetachVirtualJoystick(id), "detach virtual controller");
    service.Poll("sdl", true);
    require(input.GetState(0, &state) == X_ERROR_DEVICE_NOT_CONNECTED, "disconnect reaches the guest");
  }
  const auto vanished = SDL_AttachVirtualJoystick(&desc);
  require(vanished != 0, "short-lived controller attached");
  service.Poll("sdl", true);
  require(SDL_DetachVirtualJoystick(vanished), "disconnect before runtime drains the connection");
  service.Poll("sdl", true);
  require(input.GetState(0, &state) == X_ERROR_DEVICE_NOT_CONNECTED, "stale connection is harmless");
  window.RequestClose();
  service.Shutdown();
}
}  // namespace
int main() {
  using namespace narutorise::controls;
  auto& service = Hardware();
  service.Initialize();
  service.Poll("sdl", true);
  auto first = service.Devices();
  service.Poll("sdl", true);
  auto second = service.Devices();
  if (first->revision != second->revision || first->devices.empty() ||
      !first->devices.back().keyboard)
    return 1;
  std::cout << "Real SDL inventory: " << first->devices.size() - 1 << " gamepads\n";
  for (const auto& d : first->devices) {
    if (d.key.empty() || (d.keyboard && d.rumble))
      return 2;
    std::cout << d.name << "; rumble=" << d.rumble << "; transient=" << d.transient << '\n';
  }
  if (service.Rumble("not-a-real-device", 1, 1, 50))
    return 3;
  service.Poll("xinput", true);
  auto xinput = service.Devices();
  std::cout << "Real XInput inventory: " << xinput->devices.size() - 1 << " gamepads\n";
  for (const auto& d : xinput->devices)
    if (!d.keyboard && d.xinput < 0)
      return 4;
  service.Shutdown();
  if (!service.Devices()->devices.empty())
    return 5;
  RawState raw;
  if (!raw.Neutral())
    return 6;
  raw.axes[4] = .8;
  if (raw.Neutral())
    return 7;
  Inventory units;
  Device a, b;
  a.key = "serial:first";
  b.key = "serial:second";
  a.name = b.name = "Same model";
  units.devices = {a, b};
  if (!FindDevice(units, a.key) || FindDevice(units, "removed"))
    return 8;
  units.devices[1].key = a.key;
  if (FindDevice(units, a.key))
    return 9;
  try { AutomaticSDL(service); }
  catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 10; }
  std::cout << "PASS: actual backend inventory, stable snapshots, capabilities, invalid rumble, "
               "shutdown\n";
}
