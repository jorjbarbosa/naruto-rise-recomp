// The process's one way out (quit.h).

#include "host/quit.h"

#include "host/input.h"
#include "host/timing.h"
#include "host/window.h"
#include "kernel/kernel.h"
#include "log.h"
#include "settings/settings.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

namespace host
{
    namespace
    {
        // The longest wait for a save being written; then the quit goes on (logged).
        constexpr double kSaveWaitSeconds = 10.0;
        constexpr int kSaveWaitPollMs = 50;

        std::atomic<bool> g_quitting{ false };
    }

    void Quit(const char* reason)
    {
        if (!g_quitting.exchange(true))
            Log("quit: %s", reason);
        RequestQuit();
    }

    void QuitFromGuest(const char* reason)
    {
        Quit(reason);
        for (;;)
            std::this_thread::sleep_for(std::chrono::hours(1));
    }

    void FinishQuit()
    {
        // A save goes through host files into a package the game empties first: leaving in the
        // middle of it would leave a broken save. The window keeps being pumped, its events dropped.
        if (kernel::ContentMounted())
        {
            Log("quit: waiting for the game to close its save");
            int64_t limit = Counter() + CounterTicks(kSaveWaitSeconds);
            SDL_Event event;
            while (kernel::ContentMounted())
            {
                if (Counter() >= limit)
                {
                    Log("quit: warning: a save is still open after %.0f s, quitting anyway", kSaveWaitSeconds);
                    break;
                }
                SDL_WaitEventTimeout(&event, kSaveWaitPollMs);
            }
        }
        settings::SaveIfChanged();
        StopInput();
        kernel::PrintImportSummary();
        Log("quit: done");
        // Plain exit: guest threads are still running, so no static destructors.
        std::_Exit(0);
    }
}
