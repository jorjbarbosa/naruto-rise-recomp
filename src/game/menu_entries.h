#pragma once

// The ways into the port menu (ui/ui.h) and out of it, on the game's side:
// - The game's Options (main menu and story pause): its track creates its screen as usual, then the
//   port menu opens over it, opaque; on close one tick of the track with A forced takes the game's
//   own way out (saves its options if they changed, back to where it came from).
// - Escape or the pad's Guide button, anywhere: the game's own XN_SYS_UI pause, then the menu over the
//   blurred game; closing resumes. Not during a Bink movie (the game does not take the pause there).
// - The main menu's Exclusive Content, relabelled QUIT GAME: the quit confirmation.
// Every close saves the port's settings if they changed.
namespace game::menu
{
    // Takes the UI's handlers. Host main thread, after ui::Initialize succeeded; without it the
    // game's Options and Exclusive Content stay the game's.
    void Install();
    // Whether the game's Options was entered from the main menu (rather than the story pause): the
    // only place the game lets the Japanese voices change. Any thread.
    bool OptionsFromMainMenu();
}
