// UI-thread hardware polling, immutable snapshots for consumers on other threads.
#pragma once
#include "controls/config.h"
#include <SDL3/SDL.h>
#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>
namespace narutorise::controls {
struct RawState {
  uint16_t buttons = 0;
  std::array<double, 6> axes{};  // LX LY RX RY LT RT, Y positive up
  bool Neutral(double threshold = .35) const;
};
struct Device {
  std::string key, name;
  uint64_t generation = 0;
  bool keyboard = false, rumble = false, transient = false;
  std::array<bool, 6> axes{};
  RawState raw;
  SDL_Gamepad* pad = nullptr;  // UI thread only, never dereference from guest
  int xinput = -1;
};
struct Inventory {
  uint64_t revision = 0;
  std::vector<Device> devices;
};
// Ambiguous identities are deliberately not resolved by connection order.
const Device* FindDevice(const Inventory& inventory, const std::string& key);
const Device* SelectedDevice(const Config& config, size_t port, const Inventory& inventory);
class Service {
 public:
  void Initialize();
  void Shutdown();
  void Poll(const std::string& backend, bool focused);
  std::shared_ptr<const Inventory> Devices() const;
  bool Rumble(const std::string& key, double low, double high, int milliseconds);
  void StopRumble();
  void StopAllRumble();
  bool RumbleActive() const { return !rumble_key_.empty(); }  // UI thread
 private:
  mutable std::mutex mutex_;
  std::shared_ptr<const Inventory> inventory_ = std::make_shared<Inventory>();
  std::vector<SDL_Gamepad*> pads_;
  bool initialized_ = false;
  uint64_t revision_ = 0, generation_ = 0;
  void* xinput_ = nullptr;
  std::string rumble_key_, backend_;
  std::chrono::steady_clock::time_point rumble_until_{};
  uint64_t rumble_generation_ = 0;
  std::array<std::chrono::steady_clock::time_point, 4> xinput_retry_{};
};
Service& Hardware();
std::shared_ptr<const Config> Current();
void Publish(const Config& config);
std::string Backend();
void SetBackend(std::string backend);
}  // namespace narutorise::controls
