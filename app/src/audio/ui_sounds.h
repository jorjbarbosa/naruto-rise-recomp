#pragma once

// The game's interface sounds in our menus. The game's menus play theirs
// through the title's sound manager: the menus' script sends its sound object
// the message 10010 with a sound and a group, and that object's script plays
// it with 825A5788 (kind 1, the sound, the object; 82668BF8, 82668270). The
// interface sounds are in the menus' bank 0x3B, loaded at all times (the
// title, the menus, the loadings, the village, the pause), in group 15: the
// cursor 0003, the confirmation 0000, a changed value 0009, the pause menu's
// opening 0017 and closing 0018. B plays nothing in the game's menus; our
// menu closes with the pause menu's closing.
//
// The sound manager's table is the main thread's, without a lock: a request
// waits for the manager's update in the main step (82668A78 from 82684518,
// once a tick, after the scripts) and plays from there, as the game's own.
// Through the game's mixer the sounds follow its volumes and both audio
// layers (audio_layer).
namespace audio::ui_sounds {
enum class Sound { kMove, kConfirm, kAdjust, kOpen, kClose };
// Any thread: the sound plays at the game's next tick. A sound already
// waiting is not queued again; a request the game does not take within a
// quarter of a second (no game running yet) is dropped.
void Play(Sound sound);
}  // namespace audio::ui_sounds
