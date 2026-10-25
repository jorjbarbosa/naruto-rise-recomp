#pragma once

#include <memory>

#include <rex/system/interfaces/input.h>

// Input recording and replay for repeatable test boots (input_replay.cpp).
// REX_NATIVE_INPUT_RECORD=<file> records the controller the player uses;
// REX_NATIVE_INPUT_REPLAY=<file> plays a recording back in place of the real
// devices. Native builds use configured ports when neither is requested;
// legacy renderer paths retain the SDK's default assignment. NRINPUT 2 port
// annotations are optional, so recordings without them remain supported.
std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(bool tool_mode);
