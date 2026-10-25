// Connected devices stay connected; packet numbers change on neutralization
// and restoration even when the underlying driver kept the same held button.
#include "menu/input_gate.h"
#include <cstring>
#include <map>
#include <mutex>
#include <cmath>
namespace narutorise::menu {
namespace {
using namespace rex::input;
using rex::X_RESULT;
using rex::X_STATUS;
class GatedDriver final : public InputDriver {
 public:
  explicit GatedDriver(std::unique_ptr<InputDriver> driver) : InputDriver(nullptr, 0), driver_(std::move(driver)) {
    driver_->set_is_active_callback([this] { return is_active() && !input_blocked.load(); });
  }
  rex::X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void OnWindowAvailable(rex::ui::Window* window) override { driver_->OnWindowAvailable(window); }
  void EnumerateDevices(std::vector<DeviceInfo>& devices) override { driver_->EnumerateDevices(devices); }
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags, X_INPUT_CAPABILITIES* out) override {
    return driver_->GetDeviceCapabilities(id, flags, out);
  }
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* value) override { return driver_->SetDeviceVibration(id, value); }
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* state) override {
    const bool blocked_at_start = input_blocked.load();
    const auto result = driver_->GetDeviceState(id, state);
    if (result != X_ERROR_SUCCESS || !state) return result;
    std::lock_guard lock(mutex_);
    auto& previous = states_[uint64_t(id)];
    const bool blocked = blocked_at_start || input_blocked.load();
    if (blocked) previous.latch = true;
    if (previous.latch) {
      X_INPUT_KEYSTROKE discarded{};
      for (int i = 0; i < 64 && driver_->GetDeviceKeystroke(id, 0, &discarded) == X_ERROR_SUCCESS; ++i) {}
    }
    const auto& pad = state->gamepad;
    const bool released = !uint16_t(pad.buttons) && pad.left_trigger < 32 && pad.right_trigger < 32 &&
        std::abs(int(int16_t(pad.thumb_lx))) < 8000 && std::abs(int(int16_t(pad.thumb_ly))) < 8000 &&
        std::abs(int(int16_t(pad.thumb_rx))) < 8000 && std::abs(int(int16_t(pad.thumb_ry))) < 8000;
    if (!blocked && released) previous.latch = false;
    if (blocked || previous.latch) state->gamepad = {};
    if (!previous.packet || std::memcmp(&previous.pad, &state->gamepad, sizeof(previous.pad))) {
      previous.pad = state->gamepad; ++previous.packet;
    }
    state->packet_number = previous.packet;
    return result;
  }
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags, X_INPUT_KEYSTROKE* out) override {
    auto result = driver_->GetDeviceKeystroke(id, flags, out);
    bool blocked = input_blocked.load();
    { std::lock_guard lock(mutex_); blocked |= states_[uint64_t(id)].latch; }
    if (!blocked) return result;
    // Drain presses while blocked, so they cannot surface after closing.
    for (int i = 0; i < 64 && result == X_ERROR_SUCCESS; ++i)
      result = driver_->GetDeviceKeystroke(id, flags, out);
    if (out) *out = {};
    return result == X_ERROR_DEVICE_NOT_CONNECTED ? result : X_ERROR_EMPTY;
  }
 private:
  std::unique_ptr<InputDriver> driver_;
  struct Last { X_INPUT_GAMEPAD pad{}; uint32_t packet = 0; bool latch = false; };
  std::mutex mutex_;
  std::map<uint64_t, Last> states_;
};
}
std::unique_ptr<rex::input::InputDriver> GateInput(std::unique_ptr<rex::input::InputDriver> driver) {
  if (!input_gate_enabled.load()) return driver;
  return std::make_unique<GatedDriver>(std::move(driver));
}
}  // namespace narutorise::menu
