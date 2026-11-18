#pragma once

// The port menu's pages (port menus, phase 6): GAME, VIDEO, AUDIO and CONTROLS, built from a table of
// rows into the menu's document (the element #rows), each row a port option of the settings store
// (settings/settings.h) or one of the game's own options (game/game_options.h). Host main thread
// only; called by ui.cpp.
//
// A row changes with left and right (the "step" event), with Accept (a click) and with the mouse on
// its arrows, choices and bar; Reset puts it back to its default. The focused or hovered row is
// described in #description. A key slot of the CONTROLS page pushes rebind.rml and takes the next key.

namespace Rml
{
    class ElementDocument;
}

namespace ui::pages
{
    // A document was loaded: the menu's gets its pages (the others are left alone).
    void Attach(Rml::ElementDocument* document);
    // A root document opens or everything closes: the game's options are watched while open, and the
    // ones changed from the key's menu are saved at the close.
    void Opened();
    void Closed();
    // Each update while open: values that changed elsewhere (the game's, the window's) are shown.
    void Update();
}
