// The port menu's entries on the game's side (menu_entries.h).

#include "game/menu_entries.h"

#include "game/addresses.h"
#include "game/game_options.h"
#include "kernel/kernel.h"
#include "kernel/xam.h"
#include "log.h"
#include "memory.h"
#include "settings/settings.h"
#include "ui/ui.h"

#include <ppc_config.h>
#include <ppc_context.h>

#include <atomic>
#include <cstring>

DECLARE_ORIGINAL(AI_OPTIONS_TRACK);
DECLARE_ORIGINAL(AI_BUTTON_JUST_PRESSED);
DECLARE_ORIGINAL(AI_EXCLUSIVE_CONTENT);
DECLARE_ORIGINAL(AI_MNU_VAR_SET_TEXT);
PPC_EXTERN_FUNC(AI_MNU_VAR_SET_STRING);

namespace game::menu
{
    namespace
    {
        constexpr const char* kMenuDocument = "menu.rml";
        constexpr const char* kQuitDocument = "quit.rml";
        // How long the key waits for the game to take its pause before giving up (a Bink movie: the
        // game runs the movie's own loop, which does not read notifications).
        constexpr double kPauseWaitSeconds = 0.5;
        // The longest the closed menu stays drawn while the game takes its Options screen down.
        constexpr double kOptionsExitSeconds = 3.0;

        std::atomic<bool> g_installed{ false };

        // ---- The Options entry: the track's phases. AI thread, except g_optionsClosed. ----

        enum class OptionsPhase
        {
            Game,    // the track runs as the game's (its screen's creation)
            Open,    // the port menu is asked for or open; the track idles
            Leaving, // the menu closed: the track runs with A forced until it leaves
        };
        OptionsPhase g_optionsPhase = OptionsPhase::Game;
        uint32_t g_leavingTicks = 0;
        std::atomic<bool> g_optionsClosed{ false }; // set by the close handler (main thread)
        // The menu model's variables, and whether the forced A changed the state: the Options
        // screen's outro then runs, and the next screen's creation ends with g_2908 at 0 again.
        std::atomic<uint32_t> g_menuVariables{ 0 };
        std::atomic<bool> g_optionsLeft{ false };
        std::atomic<bool> g_fromMainMenu{ false };
        // While the track runs its way out: IO_ButtonJustPressed(0, 0) answers true.
        thread_local bool t_forceAccept = false;

        uint32_t ModelVariables()
        {
            uint32_t object = mem::Load32(mem::Load32(ai::kCurrentObject));
            return mem::Load32(object + ai::kVariables);
        }

        // ---- The key ----

        bool PauseTaken()
        {
            return mem::Load8(jade::kSystemUiTaken) != 0;
        }

        void PauseRefused()
        {
            // Taken after the movie, back to back with the 1: no visible effect (experiment 4).
            kernel::PostNotification(kernel::kXnSysUi, 0);
            Log("menu: the game did not take the pause (a movie is playing), no menu");
        }

        void OnMenuKey()
        {
            kernel::PostNotification(kernel::kXnSysUi, 1);
            if (!ui::OpenWhen(kMenuDocument, ui::Entry::Key, PauseTaken, kPauseWaitSeconds, PauseRefused))
                kernel::PostNotification(kernel::kXnSysUi, 0);
        }

        // ---- Closing ----

        // Whether the game has taken its Options screen down: the screen it went back to exists.
        bool OptionsGone()
        {
            uint32_t variables = g_menuVariables.load(std::memory_order_acquire);
            return g_optionsLeft.load(std::memory_order_acquire) && variables != 0 &&
                   mem::AtomicLoad32(variables + ai::menu::kStateEntering) == 0;
        }

        void OnClosed(ui::Entry entry)
        {
            switch (entry)
            {
            case ui::Entry::Options:
                // The menu stays drawn over the game's Options screen until it is gone, so its outro
                // is not seen (the game shows the next screen's intro instead).
                g_optionsLeft.store(false, std::memory_order_release);
                g_optionsClosed.store(true, std::memory_order_release);
                ui::HoldLastFrame(OptionsGone, kOptionsExitSeconds);
                break;
            case ui::Entry::Key:
                kernel::PostNotification(kernel::kXnSysUi, 0);
                game::options::ReapplyAfterResume();
                break;
            case ui::Entry::MainMenuQuit:
                break; // the main menu goes on as after its own call
            }
            settings::SaveIfChanged();
        }

        // ---- QUIT GAME ----

        // The label of the main menu's 5th entry, in the game's language. ASCII: the movie takes the
        // bytes as they are.
        const char* QuitLabel()
        {
            switch (settings::GameLanguage())
            {
            case settings::Language::French: return "Quitter le jeu";
            case settings::Language::Spanish: return "Salir del juego";
            case settings::Language::Italian: return "Esci dal gioco";
            case settings::Language::German: return "Spiel beenden";
            case settings::Language::English: break;
            }
            return "Quit Game";
        }

        // The label in guest memory, made once. AI thread.
        uint32_t GuestQuitLabel()
        {
            static uint32_t address = 0;
            if (address == 0)
            {
                const char* label = QuitLabel();
                uint32_t size = uint32_t(std::strlen(label)) + 1;
                address = mem::Alloc(mem::Heap::System, size);
                if (address != 0)
                    std::memcpy(mem::Ptr(address), label, size);
            }
            return address;
        }
    }

    bool OptionsFromMainMenu()
    {
        return g_fromMainMenu.load(std::memory_order_acquire);
    }

    void Install()
    {
        ui::Handlers handlers;
        handlers.closed = OnClosed;
        handlers.menuKey = OnMenuKey;
        ui::SetHandlers(handlers);
        g_installed.store(true, std::memory_order_release);
    }
}

using namespace game;
using namespace game::menu;

// The Options track (fct_9E002BAB), every tick of state 27. The game's own code creates the screen
// (while g_2908 is 1) and later leaves it; in between the port menu stands in for it.
PPC_FUNC(AI_OPTIONS_TRACK)
{
    if (!g_installed.load(std::memory_order_acquire))
    {
        ORIGINAL(AI_OPTIONS_TRACK)(ctx, base);
        return;
    }
    uint32_t variables = ModelVariables();
    g_menuVariables.store(variables, std::memory_order_release);
    // A state's first ticks: the screen's creation, or the track run as the outro of the screen that
    // follows it (from that screen's own first ticks).
    if (mem::Load32(variables + ai::menu::kStateEntering) == 1)
    {
        g_optionsPhase = OptionsPhase::Game;
        g_optionsClosed = false;
        ORIGINAL(AI_OPTIONS_TRACK)(ctx, base);
        return;
    }
    switch (g_optionsPhase)
    {
    case OptionsPhase::Game:
        // The screen exists: the port menu covers it. Its movie stays behind, reading nothing.
        g_optionsPhase = OptionsPhase::Open;
        g_fromMainMenu.store(mem::Load32(variables + ai::menu::kFromMainMenu) == 1, std::memory_order_release);
        ui::RequestOpen(kMenuDocument, ui::Entry::Options);
        ctx.r3.u64 = ai::menu::kTrackIdle;
        return;
    case OptionsPhase::Open:
        if (!g_optionsClosed.exchange(false, std::memory_order_acquire))
        {
            ctx.r3.u64 = ai::menu::kTrackIdle;
            return;
        }
        g_optionsPhase = OptionsPhase::Leaving;
        g_leavingTicks = 0;
        [[fallthrough]];
    case OptionsPhase::Leaving:
        // A, not B: B reverts the game's options to the screen's snapshot, undoing what the port's
        // pages wrote into them. A saves them if they changed and leaves to the main menu or the
        // story pause, by the game's own code.
        if (++g_leavingTicks == 2)
            Log("menu: warning: the Options track did not leave on its first forced A");
        t_forceAccept = true;
        ORIGINAL(AI_OPTIONS_TRACK)(ctx, base);
        t_forceAccept = false;
        // The way out changes the state, which sets g_2908 for the next screen.
        if (mem::Load32(variables + ai::menu::kStateEntering) == 1)
            g_optionsLeft.store(true, std::memory_order_release);
        return;
    }
}

// IO_ButtonJustPressed: the Options track's way out sees A pressed on pad 0.
PPC_FUNC(AI_BUTTON_JUST_PRESSED)
{
    if (t_forceAccept && ctx.r3.u32 == 0 && ctx.r4.u32 == 0)
    {
        ctx.r3.u64 = 1;
        return;
    }
    ORIGINAL(AI_BUTTON_JUST_PRESSED)(ctx, base);
}

// Exclusive Content, relabelled QUIT GAME: the port's quit confirmation instead of LinkApp.xex.
PPC_FUNC(AI_EXCLUSIVE_CONTENT)
{
    if (!g_installed.load(std::memory_order_acquire))
    {
        ORIGINAL(AI_EXCLUSIVE_CONTENT)(ctx, base);
        return;
    }
    ui::RequestOpen(kQuitDocument, ui::Entry::MainMenuQuit);
    ctx.r3.u64 = 0;
}

// MNU_VarSetText: Exclusive Content's label becomes QUIT GAME, through MNU_VarSetString.
PPC_FUNC(AI_MNU_VAR_SET_TEXT)
{
    if (g_installed.load(std::memory_order_acquire) && ctx.r5.u32 == ai::menu::kExclusiveContentText)
    {
        if (uint32_t label = GuestQuitLabel(); label != 0)
        {
            ctx.r5.u64 = label;
            AI_MNU_VAR_SET_STRING(ctx, base);
            return;
        }
    }
    ORIGINAL(AI_MNU_VAR_SET_TEXT)(ctx, base);
}
