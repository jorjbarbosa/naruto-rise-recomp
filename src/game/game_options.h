#pragma once

// The game's own options, for the menu pages: the 12 integers of its option store (model_24024C16,
// menu-screens.md section 7), read and written through the store's own procedures, so the clamping,
// the apply (volumes, brightness, vibration, the voice language) and the save stay the game's.
//
// The procedures run on the thread of the AI scripts, at the start of the main step
// (interpolation/engine_hooks.cpp); the menu, on the host main thread, queues its writes and reads the
// values those steps publish. Nothing is there before the store's first load, which the game makes
// after Press Start (game-side-findings.md section 6).

#include <ppc_config.h>
#include <ppc_context.h>

#include <cstdint>

namespace game::options
{
    // The options the pages show, by their index in the store.
    enum Index : int
    {
        kMusic = 0,          // 0 to 10
        kVoice = 1,          // 0 to 10
        kEffects = 2,        // 0 to 10
        kBrightness = 3,     // 6 to 26, step 2
        kInvertX = 4,        // 0 or 1
        kInvertY = 5,        // 0 or 1
        kVibration = 6,      // 0 or 1
        kSubtitles = 7,      // 0 or 1
        kJapaneseVoices = 8, // 0 or 1
        kShown = 9,          // 9 to 11 are the fight mode's, left to its own screen
    };

    // While the menu is open: the steps publish the values (and whether the Japanese voices are
    // offered). Host main thread.
    void Watch(bool watching);
    // Whether the store was loaded. Any thread.
    bool Loaded();
    // Whether the values below are the game's: loaded, and published by a step since Watch(true).
    // The pages change the game's options only then. Any thread.
    bool Ready();
    // An option's value as last published (or as last set, until a step publishes it). Any thread.
    int32_t Value(int index);
    // Queues a value for the next step; the game clamps it and applies it. Host main thread.
    void Set(int index, int32_t value);
    // Whether a value was set since the last save. Host main thread.
    bool Changed();
    // Queues a save to the profile for the next step: the game writes only once its profile manager
    // knows the user, a few frames after Press Start (the result is logged). Host main thread.
    void RequestSave();
    // At the quit: when a value changed since the last save, saves and waits for the step that does
    // it, at most that long. False when it did not happen. Host main thread.
    bool SaveBeforeQuit(double seconds);
    // The game resumes from the key's menu (XN_SYS_UI 0, which puts back the vibration switch it
    // saved at the pause): a vibration set in the menu is applied again a few steps later. Host main
    // thread.
    void ReapplyAfterResume();
    // Whether the game offers the Japanese voices (the downloadable voices are there): what its
    // Options track asks before showing their row. Any thread, once a step has published it.
    bool JapaneseVoicesOffered();

    // The start of the main step (thread of the AI scripts): the queued writes and save, then the
    // values while watched.
    void MainStep(PPCContext& ctx, uint8_t* base);
}
