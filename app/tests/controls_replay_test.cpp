#include "input_replay.h"
#include "controls/runtime.h"
#include "menu/input_gate.h"
#include <rex/input/input_system.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <chrono>
using namespace narutorise::controls;
using namespace rex::input;
using rex::X_RESULT;
using rex::X_STATUS;
void Check(bool value, const char* why) {
  if (!value)
    throw std::runtime_error(why);
}
void Env(const char* name, const std::string& value) {
#ifdef _WIN32
  _putenv_s(name, value.c_str());
#else
  if (value.empty())
    unsetenv(name);
  else
    setenv(name, value.c_str(), 1);
#endif
}
int main(int argc, char** argv) {
  if (argc != 2)
    return 2;
  try {
    const auto root = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(root);
    narutorise::menu::input_gate_enabled = true;
    narutorise::menu::input_blocked = false;
    Config config;
    config.version = 1;
    config.ports[2].mouse = false;
    Assign(config, 2, "keyboard", "Keyboard and mouse");
    Publish(config);
    const auto recorded = root / "ports.txt";
    Env("REX_NATIVE_INPUT_RECORD", recorded.string());
    {
      auto base = CreateInputSystem(false);
      auto* input = static_cast<InputSystem*>(base.get());
      X_INPUT_STATE state{};
      Check(input->GetState(0, &state) == X_ERROR_DEVICE_NOT_CONNECTED,
            "recorded None is disconnected");
      Check(input->GetState(2, &state) == X_ERROR_SUCCESS, "actual keyboard driver assigned to J3");
      Assign(config, 0, "keyboard", "Keyboard and mouse");
      Publish(config);
      Check(input->GetState(0, &state) == X_ERROR_SUCCESS, "record live transfer to J1");
    }
    Env("REX_NATIVE_INPUT_RECORD", "");
    Env("REX_NATIVE_INPUT_REPLAY_CLOCK", "reads");
    Env("REX_NATIVE_INPUT_REPLAY", recorded.string());
    {
      auto base = CreateInputSystem(false);
      auto* input = static_cast<InputSystem*>(base.get());
      X_INPUT_STATE state{};
      Check(input->GetState(0, &state) == X_ERROR_DEVICE_NOT_CONNECTED,
            "replay uses recorded J3 assignment");
      Check(input->GetState(2, &state) == X_ERROR_SUCCESS,
            "replay keyboard on J3 despite current live J1 selection");
      Check(input->GetState(2, &state) == X_ERROR_DEVICE_NOT_CONNECTED &&
                input->GetState(0, &state) == X_ERROR_SUCCESS,
            "replay preserves transfer chronology");
      input->Shutdown();
      Check(input->GetState(0, &state) == X_ERROR_DEVICE_NOT_CONNECTED,
            "replay routing expires safely on shutdown");
    }
    const auto legacy = root / "legacy.txt";
    std::ofstream(legacy) << "NRINPUT 2\nD 1 1 Keyboard fixture\nS 0 0 1 1 4096 0 0 0 0 0 0\nS 1 1 "
                             "1 2 0 0 0 0 0 0 0\n";
    Env("REX_NATIVE_INPUT_REPLAY", legacy.string());
    {
      auto base = CreateInputSystem(false);
      auto* input = static_cast<InputSystem*>(base.get());
      X_INPUT_STATE state{};
      Check(
          input->GetState(0, &state) == X_ERROR_SUCCESS && uint16_t(state.gamepad.buttons) == 4096,
          "old NRINPUT 2 keeps legacy assignment");
      Check(input->GetState(1, &state) == X_ERROR_DEVICE_NOT_CONNECTED,
            "legacy synthetic device stays J1");
    }
    const auto ports = root / "explicit.txt";
    std::ofstream(ports)
        << "NRINPUT 2\nD 7 0 Pad fixture\nD 8 1 Keyboard fixture\nP 0 0 7 3\nP 0 0 8 1\nS 0 0 7 1 "
           "8192 0 0 0 0 0 0\nS 0 0 8 1 4096 0 0 0 0 0 0\nP 100 1 7 0\nS 100 1 7 2 0 0 0 0 0 0 0\n";
    Env("REX_NATIVE_INPUT_REPLAY", ports.string());
    {
      auto base = CreateInputSystem(false);
      auto* input = static_cast<InputSystem*>(base.get());
      X_INPUT_STATE state{};
      Check(
          input->GetState(1, &state) == X_ERROR_SUCCESS && uint16_t(state.gamepad.buttons) == 4096,
          "recorded keyboard J2");
      Check(input->GetState(2, &state) == X_ERROR_DEVICE_NOT_CONNECTED, "recorded J3 None");
      Check(
          input->GetState(3, &state) == X_ERROR_SUCCESS && uint16_t(state.gamepad.buttons) == 8192,
          "recorded controller J4");
      Check(input->GetState(0, &state) == X_ERROR_SUCCESS &&
                input->GetState(3, &state) == X_ERROR_DEVICE_NOT_CONNECTED,
            "recorded controller transfers J4 to J1");
    }
    const auto timed = root / "timed.txt";
    std::ofstream(timed) << "NRINPUT 2\nD 9 0 Timed fixture\nP 0 0 9 2\nS 0 0 9 1 4096 0 0 0 0 0 "
                            "0\nP 10000 1 9 0\nS 10000 1 9 2 8192 0 0 0 0 0 0\n";
    Env("REX_NATIVE_INPUT_REPLAY_CLOCK", "time");
    Env("REX_NATIVE_INPUT_REPLAY", timed.string());
    {
      auto base = CreateInputSystem(false);
      auto* input = static_cast<InputSystem*>(base.get());
      X_INPUT_STATE state{};
      Check(
          input->GetState(2, &state) == X_ERROR_SUCCESS && uint16_t(state.gamepad.buttons) == 4096,
          "time replay starts on recorded J3");
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
      Check(input->GetState(2, &state) == X_ERROR_DEVICE_NOT_CONNECTED &&
                input->GetState(0, &state) == X_ERROR_SUCCESS &&
                uint16_t(state.gamepad.buttons) == 8192,
            "time replay preserves recorded transfer");
    }
    Env("REX_NATIVE_INPUT_REPLAY", "");
    Env("REX_NATIVE_INPUT_REPLAY_CLOCK", "");
    std::cout << "PASS: actual keyboard record/replay, four recorded ports/transfers, None, legacy "
                 "NRINPUT 2 and shutdown lifetime\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
