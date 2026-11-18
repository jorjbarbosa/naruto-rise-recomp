#pragma once

// The host UI: RmlUi documents drawn over the game through the renderer's UI bridge (gpu/ui.h).
//
// Host main thread only, unless marked: the event loop (host/window.cpp) feeds it every SDL event,
// and wakes it once per guest present while a document is open (and at least every
// WaitTimeoutMs). While a document is open the game's input is held (game/input_hold.h).
//
// Documents are laid out on the game's 1280x720 stage in dp units: the context's dp ratio is
// min(width / 1280, height / 720) of the window in pixels.
//
// The open documents are a stack: a root, opened for one entry (the way the menu was reached), and
// modal documents pushed over it. The entry picks only the backdrop (the game image blurred behind
// the key's menu), the root's way out (the close handler) and an attribute of every document's body,
// entry="options", "key" or "main-menu-quit", for the style sheets. A "back" event that no element
// stops pops the top document, and closes them all when it was the root. An element with a
// data-action attribute acts on click: "back" (the same), "push:<document>" or "quit" (host/quit.h).
// Accept (A, Enter) with no element focused clicks the top document's element with the data-accept
// attribute, if any: a window whose only choices are its A and B prompts. Left and right are first
// a "step" event (parameter "delta", -1 or 1) to the focused element: one that stops it changes a
// value, otherwise the focus moves. Every body also carries input="pad", "keyboard" or "mouse": the
// device last used, for the prompts. The menu's pages (ui/pages.h) are built into the documents that
// have their elements.

#include <filesystem>

union SDL_Event;
struct SDL_Window;

namespace ui
{
    // How the menu was reached.
    enum class Entry
    {
        Options,      // the game's Options (main menu, story pause): opaque, in place of its screen
        Key,          // Escape or the pad's Guide button: over the paused game, blurred
        MainMenuQuit, // the main menu's QUIT GAME: the quit confirmation alone
    };

    // What the menus' owner (game/menu_entries.h) does at the edges. Host main thread.
    struct Handlers
    {
        // After the documents of a root opened for this entry closed, or when a root for it could not
        // be opened: the entry's way out.
        void (*closed)(Entry entry) = nullptr;
        // Escape or the Guide button with no document open and none about to open.
        void (*menuKey)() = nullptr;
    };

    // The library, the fonts of uiDir/fonts and the images of gameDir/Naruto_clip.SRA. False when the
    // UI cannot run (logged); the game runs without it.
    bool Initialize(SDL_Window* window, const std::filesystem::path& uiDir, const std::filesystem::path& gameDir);
    // Whether Initialize succeeded. Any thread.
    bool IsAvailable();
    void SetHandlers(const Handlers& handlers);

    // Every event of the loop, before the game's input sees it (which it still does).
    void HandleEvent(const SDL_Event& event);
    // The wake event of the presents, or a wait that timed out: opens what was asked for, updates
    // and renders the open documents, and hides an idle cursor.
    void Update();
    // How long the loop may wait for an event before calling Update: -1 for no limit.
    int WaitTimeoutMs();
    // Whether an event is the UI's wake event (Update is then all it needs).
    bool IsWakeEvent(const SDL_Event& event);

    // Opens a document under uiDir as the root for an entry, closing what is open. False when it
    // cannot be loaded (logged; the close handler then runs for the entry).
    bool Open(const char* document, Entry entry);
    // Shows a document modal over the top one. False when none is open or it cannot be loaded.
    bool Push(const char* document);
    // Closes every document, then runs the close handler for the root's entry.
    void Close();
    // Closes the top document, as a "back" no element stops.
    void Back();
    // The next key pressed (not a repeat) goes to the handler, as an SDL scancode, instead of the
    // menu; dropped when a document opens or closes.
    void CaptureKey(void (*handler)(int scancode));
    // The root's entry; meaningful while IsOpen.
    Entry CurrentEntry();
    // From the close handler: the documents' last frame stays drawn until done() returns true
    // (polled about every 16 ms) or timeoutSeconds pass, and the game's input stays held: the game
    // takes down what it showed behind them first.
    void HoldLastFrame(bool (*done)(), double timeoutSeconds);
    // Any thread.
    bool IsOpen();

    // Any thread: Open at the main thread's next turn. A request while a document is open or another
    // request is pending is refused (logged; the close handler runs for its entry on the main
    // thread).
    void RequestOpen(const char* document, Entry entry);
    // Opens a root once ready() returns true, polled about every 16 ms; after timeoutSeconds without
    // it, gives up and calls expired() instead. One at a time: refused (false) while a document is
    // open or another open is pending.
    bool OpenWhen(const char* document, Entry entry, bool (*ready)(), double timeoutSeconds, void (*expired)());
}
