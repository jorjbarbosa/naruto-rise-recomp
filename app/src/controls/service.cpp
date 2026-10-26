#include "controls/service.h"
#include <rex/input/sdl/device_identity.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/cvar.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#include <xinput.h>
#endif
namespace narutorise::controls {
namespace {
std::atomic<std::shared_ptr<const Config>> current{std::make_shared<Config>()};
std::mutex backend_mutex;
std::string selected_backend = "sdl";
}  // namespace
Service& Hardware() {
  static Service service;
  return service;
}
const Device* FindDevice(const Inventory& inventory, const std::string& key) {
  const Device* found = nullptr;
  for (const auto& d : inventory.devices)
    if (d.key == key) {
      if (found)
        return nullptr;
      found = &d;
    }
  return found;
}
const Device* SelectedDevice(const Config& config, size_t port, const Inventory& inventory) {
  if (port >= kPorts) return nullptr;
  const auto& key = config.ports[port].device;
  if (key != kAutomaticDevice) return FindDevice(inventory, key);
  std::vector<std::string> pads;
  for (const auto& device : inventory.devices)
    if (!device.keyboard) pads.push_back(device.key);
  if (const auto selected = AutomaticPad(config, pads); !selected.empty())
    return FindDevice(inventory, selected);
  return AvailableForAutomatic(config, "keyboard") ? FindDevice(inventory, "keyboard") : nullptr;
}
std::shared_ptr<const Config> Current() {
  return current.load();
}
void Publish(const Config& config) {
  if (!Valid(config))
    return;
  current.store(std::make_shared<const Config>(config));
  if (!config.version) {
    rex::input::mnk::SetProfile({});
    return;
  }
  auto profile = std::make_shared<rex::input::mnk::Profile>();
  size_t owner = kPorts;
  for (size_t i = 0; i < kPorts; ++i)
    if (config.ports[i].device == "keyboard") owner = i;
  if (owner == kPorts)
    for (size_t i = 0; i < kPorts; ++i)
      if (config.ports[i].device == kAutomaticDevice) owner = i;
  if (owner < kPorts) {
    const auto& port = config.ports[owner];
    profile->enabled = true;
    profile->user = uint32_t(owner);
    profile->mouse = port.mouse;
    profile->sensitivity = port.sensitivity;
    profile->bindings = port.keys;
  }
  rex::input::mnk::SetProfile(std::move(profile));
}
std::string Backend() {
  std::lock_guard lock(backend_mutex);
  return selected_backend;
}
void SetBackend(std::string backend) {
  std::lock_guard lock(backend_mutex);
  selected_backend = std::move(backend);
}
bool RawState::Neutral(double threshold) const {
  return !buttons &&
         std::all_of(axes.begin(), axes.end(), [=](double v) { return std::abs(v) < threshold; });
}
void Service::Initialize() {
  if (initialized_)
    return;
  initialized_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
  // Use the same real controller database as the runtime before the guest
  // exists, so custom-mapped devices can already be selected in setup.
  if (initialized_) {
    const auto mappings = rex::cvar::GetFlagByName("hid_mappings_file");
    std::error_code error;
    if (!mappings.empty() && std::filesystem::exists(std::filesystem::u8path(mappings), error))
      SDL_AddGamepadMappingsFromFile(mappings.c_str());
  }
#ifdef _WIN32
  if (!xinput_)
    xinput_ = LoadLibraryW(L"xinput1_4.dll");
#endif
}
std::shared_ptr<const Inventory> Service::Devices() const {
  std::lock_guard lock(mutex_);
  return inventory_;
}
void Service::Shutdown() {
  StopAllRumble();
  for (auto* pad : pads_)
    SDL_CloseGamepad(pad);
  pads_.clear();
  if (initialized_)
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
  initialized_ = false;
#ifdef _WIN32
  if (xinput_)
    FreeLibrary(static_cast<HMODULE>(xinput_));
#endif
  xinput_ = nullptr;
  std::lock_guard lock(mutex_);
  inventory_ = std::make_shared<Inventory>();
}
void Service::Poll(const std::string& backend, bool focused) {
  Initialize();
  if (!focused || backend != backend_ || std::chrono::steady_clock::now() >= rumble_until_)
    StopRumble();
  backend_ = backend;
  const auto before = Devices();
  auto next = std::make_shared<Inventory>();
  if (initialized_) {
    SDL_PumpEvents();
    SDL_UpdateGamepads();
    std::erase_if(pads_, [](SDL_Gamepad* pad) {
      if (SDL_GamepadConnected(pad))
        return false;
      SDL_RumbleGamepad(pad, 0, 0, 0);
      SDL_CloseGamepad(pad);
      return true;
    });
    int count = 0;
    auto* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i)
      if (std::none_of(pads_.begin(), pads_.end(),
                       [&](auto* p) { return SDL_GetGamepadID(p) == ids[i]; }))
        if (auto* p = SDL_OpenGamepad(ids[i]))
          pads_.push_back(p);
    SDL_free(ids);
    if (backend != "xinput")
      for (auto* pad : pads_) {
        Device d;
        d.key = rex::input::sdl::DeviceKey(pad);
        d.pad = pad;
        const char* name = SDL_GetGamepadName(pad);
        d.name = name ? name : "SDL Gamepad";
        d.generation = SDL_GetGamepadID(pad);
        d.transient = d.key.starts_with("sdl:session:");
        d.rumble = SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad),
                                          SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
        constexpr SDL_GamepadButton buttons[] = {
            SDL_GAMEPAD_BUTTON_DPAD_UP,       SDL_GAMEPAD_BUTTON_DPAD_DOWN,
            SDL_GAMEPAD_BUTTON_DPAD_LEFT,     SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
            SDL_GAMEPAD_BUTTON_START,         SDL_GAMEPAD_BUTTON_BACK,
            SDL_GAMEPAD_BUTTON_LEFT_STICK,    SDL_GAMEPAD_BUTTON_RIGHT_STICK,
            SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
            SDL_GAMEPAD_BUTTON_GUIDE,         SDL_GAMEPAD_BUTTON_SOUTH,
            SDL_GAMEPAD_BUTTON_EAST,          SDL_GAMEPAD_BUTTON_WEST,
            SDL_GAMEPAD_BUTTON_NORTH};
        constexpr uint16_t bits[] = {1,     2,     4,     8,      0x10,   0x20,   0x40,  0x80,
                                     0x100, 0x200, 0x400, 0x1000, 0x2000, 0x4000, 0x8000};
        for (int i = 0; i < 15; ++i)
          if (SDL_GetGamepadButton(pad, buttons[i]))
            d.raw.buttons |= bits[i];
        if (rex::cvar::GetFlagByName("guide_button") != "true")
          d.raw.buttons &= ~uint16_t(0x400);
        for (int i = 0; i < 6; ++i) {
          d.axes[i] = SDL_GamepadHasAxis(pad, SDL_GamepadAxis(i));
          const int v = SDL_GetGamepadAxis(pad, SDL_GamepadAxis(i));
          d.raw.axes[i] = std::clamp(v / (v < 0 ? 32768.0 : 32767.0), -1.0, 1.0);
          if (i == 1 || i == 3)
            d.raw.axes[i] = -d.raw.axes[i];
        }
        next->devices.push_back(std::move(d));
      }
  }
#ifdef _WIN32
  if (backend == "xinput" && xinput_) {
    auto state = reinterpret_cast<decltype(&XInputGetState)>(
        GetProcAddress(static_cast<HMODULE>(xinput_), "XInputGetState"));
    auto caps = reinterpret_cast<decltype(&XInputGetCapabilities)>(
        GetProcAddress(static_cast<HMODULE>(xinput_), "XInputGetCapabilities"));
    for (int slot = 0; state && caps && slot < 4; ++slot) {
      const auto now = std::chrono::steady_clock::now();
      if (now < xinput_retry_[slot])
        continue;
      XINPUT_STATE s{};
      XINPUT_CAPABILITIES c{};
      if (state(slot, &s) != ERROR_SUCCESS) {
        xinput_retry_[slot] = now + std::chrono::milliseconds(500);
        continue;
      }
      Device d;
      d.key = "xinput:slot:" + std::to_string(slot);
      d.name = "XInput · " + std::to_string(slot + 1);
      d.xinput = slot;
      auto old = std::find_if(before->devices.begin(), before->devices.end(),
                              [&](const Device& x) { return x.key == d.key; });
      d.generation = old == before->devices.end() ? ++generation_ : old->generation;
      if (old != before->devices.end()) {
        d.rumble = old->rumble;
        d.axes = old->axes;
      } else {
        if (caps(slot, 0, &c) != ERROR_SUCCESS)
          continue;
        d.rumble = (c.Flags & XINPUT_CAPS_FFB_SUPPORTED) || c.Vibration.wLeftMotorSpeed ||
                   c.Vibration.wRightMotorSpeed;
        d.axes = {c.Gamepad.sThumbLX != 0,     c.Gamepad.sThumbLY != 0,
                  c.Gamepad.sThumbRX != 0,     c.Gamepad.sThumbRY != 0,
                  c.Gamepad.bLeftTrigger != 0, c.Gamepad.bRightTrigger != 0};
      }
      d.raw.buttons = s.Gamepad.wButtons;
      const int axes[] = {s.Gamepad.sThumbLX, s.Gamepad.sThumbLY, s.Gamepad.sThumbRX,
                          s.Gamepad.sThumbRY};
      for (int i = 0; i < 4; ++i)
        d.raw.axes[i] = axes[i] / (axes[i] < 0 ? 32768.0 : 32767.0);
      d.raw.axes[4] = s.Gamepad.bLeftTrigger / 255.0;
      d.raw.axes[5] = s.Gamepad.bRightTrigger / 255.0;
      next->devices.push_back(std::move(d));
    }
  }
#endif
  Device keyboard;
  keyboard.key = "keyboard";
  keyboard.name = "Keyboard and mouse";
  keyboard.keyboard = true;
  keyboard.generation = 1;
  next->devices.push_back(std::move(keyboard));
  for (auto& d : next->devices)
    if (!d.keyboard && d.xinput < 0)
      d.name += " · " + std::to_string(d.generation);
  bool changed = next->devices.size() != before->devices.size();
  for (size_t i = 0; !changed && i < next->devices.size(); ++i) {
    const auto& a = next->devices[i];
    const auto& b = before->devices[i];
    changed = a.key != b.key || a.generation != b.generation || a.rumble != b.rumble ||
              a.axes != b.axes || a.name != b.name;
  }
  if (changed)
    ++revision_;
  next->revision = revision_;
  // Distinguish same-model units while retaining their actual device name.
  {
    std::lock_guard lock(mutex_);
    inventory_ = next;
  }
  if (!rumble_key_.empty()) {
    const auto* device = FindDevice(*next, rumble_key_);
    if (!device || device->generation != rumble_generation_)
      StopRumble();
  }
}
bool Service::Rumble(const std::string& key, double low, double high, int milliseconds) {
  StopRumble();
  if (!std::isfinite(low) || !std::isfinite(high))
    return false;
  auto inventory = Devices();
  if (!FindDevice(*inventory, key))
    return false;
  auto it = std::find_if(inventory->devices.begin(), inventory->devices.end(),
                         [&](const Device& d) { return d.key == key; });
  if (it == inventory->devices.end() || !it->rumble)
    return false;
  const uint16_t l = uint16_t(std::clamp(low, 0.0, 1.0) * 65535),
                 h = uint16_t(std::clamp(high, 0.0, 1.0) * 65535);
  bool ok = false;
  if (it->pad)
    ok = SDL_RumbleGamepad(it->pad, l, h, std::clamp(milliseconds, 1, 5000));
#ifdef _WIN32
  if (it->xinput >= 0 && xinput_) {
    auto set = reinterpret_cast<decltype(&XInputSetState)>(
        GetProcAddress(static_cast<HMODULE>(xinput_), "XInputSetState"));
    XINPUT_VIBRATION value{l, h};
    ok = set && set(it->xinput, &value) == ERROR_SUCCESS;
  }
#endif
  if (ok) {
    rumble_key_ = key;
    rumble_generation_ = it->generation;
    rumble_until_ = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(std::clamp(milliseconds, 1, 5000));
  } else {
    if (it->pad)
      SDL_RumbleGamepad(it->pad, 0, 0, 0);
#ifdef _WIN32
    if (it->xinput >= 0 && xinput_) {
      auto set = reinterpret_cast<decltype(&XInputSetState)>(
          GetProcAddress(static_cast<HMODULE>(xinput_), "XInputSetState"));
      XINPUT_VIBRATION zero{};
      if (set)
        set(it->xinput, &zero);
    }
#endif
  }
  return ok;
}
void Service::StopRumble() {
  if (rumble_key_.empty())
    return;
  auto inventory = Devices();
  for (const auto& d : inventory->devices)
    if (d.key == rumble_key_) {
      // A disconnected handle may already have closed during this poll.
      if (d.pad && std::find(pads_.begin(), pads_.end(), d.pad) != pads_.end())
        SDL_RumbleGamepad(d.pad, 0, 0, 0);
#ifdef _WIN32
      if (d.xinput >= 0 && xinput_) {
        auto set = reinterpret_cast<decltype(&XInputSetState)>(
            GetProcAddress(static_cast<HMODULE>(xinput_), "XInputSetState"));
        XINPUT_VIBRATION value{};
        if (set)
          set(d.xinput, &value);
      }
#endif
    }
  rumble_key_.clear();
}
void Service::StopAllRumble() {
  StopRumble();
  auto inventory = Devices();
  auto config = Current();
  for (size_t i = 0; i < kPorts; ++i)
    if (const auto* d = SelectedDevice(*config, i, *inventory)) {
      if (d->pad && std::find(pads_.begin(), pads_.end(), d->pad) != pads_.end())
        SDL_RumbleGamepad(d->pad, 0, 0, 0);
#ifdef _WIN32
      if (d->xinput >= 0 && xinput_) {
        auto set = reinterpret_cast<decltype(&XInputSetState)>(
            GetProcAddress(static_cast<HMODULE>(xinput_), "XInputSetState"));
        XINPUT_VIBRATION zero{};
        if (set)
          set(d->xinput, &zero);
      }
#endif
    }
}
}  // namespace narutorise::controls
