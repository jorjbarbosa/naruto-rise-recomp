// roan-xenon: runs the recompiled default.xex of Naruto: Rise of a Ninja.
//
// usage: roan-xenon [game]   (default: <repository>/game)
// game: the game folder, its default.xex, the Xbox 360 disc image or a ZArchive (vfs::Mount).
// ROAN_TRACE=1 logs every traced kernel call.
// ROAN_REPLAY=<recording> ROAN_REPLAY_OUT=<folder> draws a recorded frame instead (gpu/replay.h).

#include "apu/xma.h"
#include "game/game_options.h"
#include "game/menu_entries.h"
#include "gpu/replay.h"
#include "host/debug.h"
#include "host/input.h"
#include "host/paths.h"
#include "host/quit.h"
#include "host/thread.h"
#include "host/window.h"
#include "kernel/kernel.h"
#include "loader.h"
#include "settings/settings.h"
#include "ui/ui.h"
#include "vfs/vfs.h"

#include <cstdlib>
#include <filesystem>
#include <memory>

PPC_EXTERN_FUNC(_xstart);

namespace fs = std::filesystem;

namespace
{
    constexpr int kCrashFrames = 40;

    // Logs the crash, the guest thread it happened on and the host stack with symbols (also the guest
    // call stack), then the import summary, and ends the process.
    void OnCrash(const host::Crash& crash)
    {
        auto* thread = kernel::CurrentThread();
        Log("crash: %s at %p (guest thread %u%s%s)", crash.description, crash.address, thread ? thread->id : 0,
            thread && !thread->name.empty() ? ", " : "", thread ? thread->name.c_str() : "");
        if (crash.memoryFault)
        {
            auto* address = static_cast<const uint8_t*>(crash.data);
            const char* access = crash.write ? "write" : "read";
            if (address >= mem::g_base && address < mem::g_base + mem::kSize * 2)
                Log("crash: %s at guest address %08llX", access, (unsigned long long)(address - mem::g_base));
            else
                Log("crash: %s at host address %p", access, address);
        }
        uint64_t frames[kCrashFrames];
        int count = host::WalkCrashStack(crash, frames, kCrashFrames);
        char symbol[320];
        for (int i = 0; i < count; i++)
        {
            host::FormatSymbol(frames[i], symbol, sizeof(symbol));
            Log("  #%-2d %s", i, symbol);
        }
        kernel::PrintImportSummary();
        std::_Exit(1);
    }

    // Host thread of the guest main thread. The host main thread runs the window.
    void GuestMain()
    {
        kernel::StartKernelClock();
        Log("time base: %llu Hz", (unsigned long long)kernel::TimeBaseFrequency());
        Log("running _xstart");
        kernel::RunMainThread(_xstart, kernel::g_module.stackSize);
        Log("_xstart returned");
        host::Quit("_xstart returned");
    }
}

int main(int argc, char** argv)
{
    g_trace = getenv("ROAN_TRACE") != nullptr;

    fs::path exeDir = host::ExecutableDirectory();
    fs::path gamePath = argc > 1 ? fs::path(argv[1]) : exeDir / ".." / ".." / "game";
    std::error_code error;
    if (fs::path canonical = fs::weakly_canonical(gamePath, error); !error)
        gamePath = canonical;
    // The game's files, for the whole run: guest threads read them until the process ends.
    std::unique_ptr<vfs::Source> game = vfs::Mount(gamePath);
    if (game == nullptr)
    {
        Log("error: no game at %s (pass the game folder, its default.xex, the disc image or a ZArchive as the "
            "first argument)", host::PathText(gamePath).c_str());
        return 1;
    }

    host::InstallCrashHandler(OnCrash);

    // A recorded frame drawn instead of the game (gpu/replay.h).
    if (const char* replay = getenv("ROAN_REPLAY"))
    {
        const char* output = getenv("ROAN_REPLAY_OUT");
        if (output == nullptr)
        {
            Log("error: ROAN_REPLAY needs ROAN_REPLAY_OUT, the folder its images go to");
            return 1;
        }
        // The images depend on the environment only, never on the user's settings file.
        settings::LoadDefaults();
        if (!host::CreateMainWindow("roan-xenon replay"))
            return 1;
        std::_Exit(gpu::RunReplay(replay, output));
    }

    if (!mem::Init())
    {
        Log("error: cannot reserve the 4 GB guest address space");
        return 1;
    }

    fs::path userDir = exeDir / "data" / "user";
    kernel::SetFileSystem(*game, userDir);
    // Before the executable, the window, the renderer and the guest read any setting.
    settings::Load(userDir / "settings.toml");
    if (!LoadExecutable(*game))
        return 1;
    // XAudioInitialize reads the XMA context base early in the title's startup.
    apu::InitializeXma();

    if (!host::CreateMainWindow("roan-xenon"))
        return 1;
    settings::SetDisplayRefreshSource(host::DisplayRefreshMillihertz);
    // Without it the game runs with no menus of the port's (logged), and its own Options.
    if (ui::Initialize(host::MainWindow(), exeDir / "data" / "ui", *game))
        game::menu::Install();

    if (!host::StartDetachedThread(host::kRecompiledStackSize, GuestMain))
    {
        Log("error: cannot create the guest main thread");
        return 1;
    }

    if (host::RunEventLoop())
        host::Quit("window closed");
    // The game's options changed in the menu and not saved yet (a quit with the menu open): the game
    // saves them on its own thread, which still runs unless it is the one quitting.
    game::options::SaveBeforeQuit(2.0);
    // Every exit ends here: the window's, the menu's and the guest's (host/quit.h).
    host::FinishQuit();
}
