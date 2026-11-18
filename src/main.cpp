// roan-xenon: runs the recompiled default.xex of Naruto: Rise of a Ninja.
//
// usage: roan-xenon [game folder]   (default: <repository>/game)
// ROAN_TRACE=1 logs every traced kernel call.
// ROAN_REPLAY=<recording> ROAN_REPLAY_OUT=<folder> draws a recorded frame instead (gpu/replay.h).

#include "apu/xma.h"
#include "gpu/replay.h"
#include "host/debug.h"
#include "host/input.h"
#include "host/paths.h"
#include "host/thread.h"
#include "host/window.h"
#include "kernel/kernel.h"
#include "loader.h"

#include <cstdlib>
#include <filesystem>

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
        host::RequestQuit();
    }
}

int main(int argc, char** argv)
{
    g_trace = getenv("ROAN_TRACE") != nullptr;

    fs::path exeDir = host::ExecutableDirectory();
    fs::path gameDir = argc > 1 ? fs::path(argv[1]) : exeDir / ".." / ".." / "game";
    std::error_code error;
    gameDir = fs::weakly_canonical(gameDir, error);
    if (!fs::exists(gameDir / "default.xex"))
    {
        Log("error: default.xex not found in %s (pass the game folder as the first argument)", gameDir.string().c_str());
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
        if (!host::CreateMainWindow("roan-xenon replay", 1280, 720))
            return 1;
        std::_Exit(gpu::RunReplay(replay, output));
    }

    if (!mem::Init())
    {
        Log("error: cannot reserve the 4 GB guest address space");
        return 1;
    }

    kernel::SetPaths(gameDir.string(), (exeDir / "data" / "user").string());
    if (!LoadExecutable(gameDir / "default.xex"))
        return 1;
    // XAudioInitialize reads the XMA context base early in the title's startup.
    apu::InitializeXma();

    if (!host::CreateMainWindow("roan-xenon", 1280, 720))
        return 1;

    if (!host::StartDetachedThread(host::kRecompiledStackSize, GuestMain))
    {
        Log("error: cannot create the guest main thread");
        return 1;
    }

    if (host::RunEventLoop())
        Log("window closed");
    host::StopInput();
    kernel::PrintImportSummary();
    // Plain exit: guest threads are still running, so no static destructors.
    std::_Exit(0);
}
