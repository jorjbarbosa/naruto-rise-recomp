// When the setup screen opens (src/menu/setup_gate.h): every combination of
// the facts against the order of the rules.
// Usage: narutorise_setup_gate_test

#include <iostream>
#include <stdexcept>
#include <string>

#include "menu/setup_gate.h"

using namespace narutorise::menu;
using narutorise::game_image::Status;
namespace {
unsigned checks = 0;
void Check(bool value, const std::string& label) {
  ++checks;
  if (!value) throw std::runtime_error(label);
}

// The rules in order, written out apart from SetupReasonFor.
SetupReason Expected(const SetupFacts& f) {
  if (f.command_line_root && !f.show_startup_menu && !f.requested) return SetupReason::kNone;
  if (!f.found && f.configured) return SetupReason::kGameMissing;
  if (!f.found) return SetupReason::kFirstRun;
  if (f.status != Status::kReady) return SetupReason::kGameNotReady;
  if (f.start_failed) return SetupReason::kStartFailed;
  if (f.requested) return SetupReason::kRequested;
  if (f.show_startup_menu) return SetupReason::kStartup;
  return SetupReason::kNone;
}
}  // namespace

int main() {
  try {
    const Status statuses[] = {Status::kReady, Status::kOtherVersion, Status::kOtherGame, Status::kNotXbox360,
                               Status::kUnreadable, Status::kIncomplete};
    for (unsigned bits = 0; bits < 64; ++bits)
      for (Status status : statuses) {
        SetupFacts f;
        f.command_line_root = bits & 1;
        f.configured = bits & 2;
        f.found = bits & 4;
        f.start_failed = bits & 8;
        f.requested = bits & 16;
        f.show_startup_menu = bits & 32;
        f.status = status;
        Check(SetupReasonFor(f) == Expected(f), "facts " + std::to_string(bits) + " status " + std::to_string(int(status)));
      }
    // The cases by name.
    SetupFacts ready;
    ready.configured = ready.found = true;
    ready.status = Status::kReady;
    Check(SetupReasonFor(ready) == SetupReason::kStartup, "ready opens the initial menu by default");
    ready.show_startup_menu = false;
    Check(SetupReasonFor(ready) == SetupReason::kNone, "disabled preference boots directly");
    SetupFacts boot = ready;
    boot.command_line_root = true;
    boot.start_failed = true;
    boot.found = false;
    Check(SetupReasonFor(boot) == SetupReason::kNone, "command line with opt-out boots directly");
    boot = ready; boot.command_line_root = true; boot.requested = true;
    Check(SetupReasonFor(boot) == SetupReason::kRequested, "explicit setup overrides the opt-out");
    SetupFacts first;
    Check(SetupReasonFor(first) == SetupReason::kFirstRun, "first run");
    SetupFacts moved;
    moved.configured = true;
    Check(SetupReasonFor(moved) == SetupReason::kGameMissing, "game moved");
    SetupFacts other = ready;
    other.status = Status::kOtherVersion;
    other.start_failed = true;
    Check(SetupReasonFor(other) == SetupReason::kGameNotReady, "the game comes before the failed start");
    SetupFacts failed = ready;
    failed.start_failed = failed.requested = true;
    Check(SetupReasonFor(failed) == SetupReason::kStartFailed, "failed start");
    SetupFacts shift = ready;
    shift.requested = true;
    Check(SetupReasonFor(shift) == SetupReason::kRequested, "Shift or --setup");
    for (SetupReason r : {SetupReason::kNone, SetupReason::kFirstRun, SetupReason::kGameMissing,
                          SetupReason::kGameNotReady, SetupReason::kStartFailed, SetupReason::kRequested, SetupReason::kStartup})
      Check(std::string(SetupReasonName(r)) != "unknown", "name");
  } catch (const std::exception& e) {
    std::cerr << "FAIL after " << checks << " checks: " << e.what() << "\n";
    return 1;
  }
  std::cout << "PASS: " << checks << " checks\n";
  return 0;
}
