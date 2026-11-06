#pragma once

// The game's own volumes (its Options screen: music, voices and effects, 0 to
// 10), read and written from the settings at any time. The script modules
// report where the game keeps its options (game_options.h). A change goes into
// that array at once and, as the game's fn_Option_Apply does it, to the game's
// group volumes (825A55F0: music group 0, voices 2, effects 1, 3, 4 and 5) at
// the next tick of the DARE (hooks.cpp); a second after the last change the
// game's own save (825BC508) writes the options into the player's profile, on
// the render thread. Without a player yet (the title before START) the save
// fails, and it is tried again every two seconds until it succeeds.
#include <array>
#include <cstdint>
#include <optional>

struct PPCContext;

namespace audio::game_volumes {
enum Volume { kMusic, kVoices, kEffects, kCount };
constexpr int kMaximum = 10;

// The volumes, once the game read its options; Set is false before.
std::optional<std::array<int, kCount>> Read();
bool Set(Volume volume, int value);

// hooks.cpp, in the DARE tick: the changes into the game.
void ApplyPending(PPCContext& ctx, uint8_t* base);
// render.cpp, between two periods on its guest thread: the save, when due.
void SaveIfDue();
}  // namespace audio::game_volumes
