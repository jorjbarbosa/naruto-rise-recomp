// The native overlay neutralizes guest input without pausing emulation. The
// driver boundary covers SDL, XInput, keyboard/mouse and recorded replays.
#pragma once
#include <atomic>
#include <memory>
#include <rex/input/input_driver.h>
namespace narutorise::menu {
inline std::atomic<bool> input_gate_enabled{false}, input_blocked{false};
std::unique_ptr<rex::input::InputDriver> GateInput(std::unique_ptr<rex::input::InputDriver> driver);
}  // namespace narutorise::menu
