// narutorise - the executable's session of the game's options (game_options.h),
// exported by name for the script modules.
#include "game_options.h"

#include <utility>

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(game_options_settings, true, "narutorise",
                    "The game's Options screen opens the in-game settings in its place, with the game's options first");

#if defined(_WIN32)
#define NARUTORISE_EXPORTED extern "C" __declspec(dllexport)
#else
#define NARUTORISE_EXPORTED extern "C" __attribute__((visibility("default")))
#endif

namespace narutorise::game_options {
namespace {
Session session;
std::function<void(const Values&)> listener;
void Opened(const Values& values) {
  if (listener) listener(values);
}
}  // namespace

Session& Shared() { return session; }

void Enable(std::function<void(const Values&)> opened) {
  if (!REXCVAR_GET(game_options_settings)) return;
  listener = std::move(opened);
  session.Enable(&Opened);
}

}  // namespace narutorise::game_options

// Linked into the executable: the input gate uses Shared().
NARUTORISE_EXPORTED narutorise::game_options::Session* narutorise_game_options() {
  return &narutorise::game_options::Shared();
}
