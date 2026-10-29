// The initial menu opens by default. Players may turn it off under Advanced;
// missing game data, a failed start or an explicit --setup still open it.
// Automated boots can pass --show_startup_menu=false with --game_data_root.
#pragma once

#include "game_image.h"

namespace narutorise::menu {

enum class SetupReason {
  kNone,           // Boot the game.
  kFirstRun,       // No game configured, and none next to the executable.
  kGameMissing,    // The configured game file or folder is gone.
  kGameNotReady,   // The game data is not the game of this port (game_image::Status).
  kStartFailed,    // The last start ended before its first frame.
  kRequested,      // Shift held at the start, or --setup.
  kStartup,        // The saved preference to show the initial menu.
};

struct SetupFacts {
  bool command_line_root = false;  // --game_data_root was given.
  bool configured = false;         // custom_game_root is set.
  bool found = false;              // A game data root exists, configured or next to the executable.
  game_image::Status status = game_image::Status::kUnreadable;  // Of the root found.
  bool start_failed = false;       // The launch marker of the last start is there.
  bool requested = false;
  bool show_startup_menu = true;
};

// The game's problems come first: the screen opens on the game file then.
SetupReason SetupReasonFor(const SetupFacts& facts);
const char* SetupReasonName(SetupReason reason);
// Shift is held now (the safe mode at the start); false where it cannot be known.
bool SetupKeyHeld();

}  // namespace narutorise::menu
