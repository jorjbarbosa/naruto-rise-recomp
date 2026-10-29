// When the setup screen opens (setup_gate.h).
#include "menu/setup_gate.h"
#ifdef _WIN32
#include <windows.h>
#endif

namespace narutorise::menu {

SetupReason SetupReasonFor(const SetupFacts& facts) {
  if (facts.command_line_root && !facts.show_startup_menu && !facts.requested) return SetupReason::kNone;
  if (!facts.found) return facts.configured ? SetupReason::kGameMissing : SetupReason::kFirstRun;
  if (facts.status != game_image::Status::kReady) return SetupReason::kGameNotReady;
  if (facts.start_failed) return SetupReason::kStartFailed;
  if (facts.requested) return SetupReason::kRequested;
  if (facts.show_startup_menu) return SetupReason::kStartup;
  return SetupReason::kNone;
}

const char* SetupReasonName(SetupReason reason) {
  switch (reason) {
    case SetupReason::kNone: return "none";
    case SetupReason::kFirstRun: return "first-run";
    case SetupReason::kGameMissing: return "game-missing";
    case SetupReason::kGameNotReady: return "game-not-ready";
    case SetupReason::kStartFailed: return "start-failed";
    case SetupReason::kRequested: return "requested";
    case SetupReason::kStartup: return "startup";
  }
  return "unknown";
}

bool SetupKeyHeld() {
#ifdef _WIN32
  return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
#else
  return false;
#endif
}

}  // namespace narutorise::menu
