#pragma once

// The host UI: RmlUi documents drawn over the game through the renderer's UI bridge (gpu/ui.h).
//
// Host main thread only, unless marked: the event loop (host/window.cpp) feeds it every SDL event,
// and wakes it once per guest present while a document is open (and at least every
// WaitTimeoutMs). While a document is open the game's input is held (game/input_hold.h).
//
// Documents are laid out on the game's 1280x720 stage in dp units: the context's dp ratio is
// min(width / 1280, height / 720) of the window in pixels.

#include <filesystem>

union SDL_Event;
struct SDL_Window;

namespace ui
{
    // The library, the fonts of uiDir/fonts and the images of gameDir/Naruto_clip.SRA. False when the
    // UI cannot run (logged); the game runs without it.
    bool Initialize(SDL_Window* window, const std::filesystem::path& uiDir, const std::filesystem::path& gameDir);

    // Every event of the loop, before the game's input sees it (which it still does).
    void HandleEvent(const SDL_Event& event);
    // The wake event of the presents, or a wait that timed out: updates and renders the open
    // document, and hides an idle cursor.
    void Update();
    // How long the loop may wait for an event before calling Update: -1 for no limit.
    int WaitTimeoutMs();
    // Whether an event is the UI's wake event (Update is then all it needs).
    bool IsWakeEvent(const SDL_Event& event);

    // Opens a document under uiDir in place of the one open, if any. False when it cannot be loaded.
    bool Open(const char* document);
    void Close();
    // Any thread.
    bool IsOpen();
}
