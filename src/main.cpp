// roan-xenon: runs the recompiled default.xex of Naruto: Rise of a Ninja.
//
// usage: roan-xenon [game folder]   (default: <repository>/game)
// ROAN_TRACE=1 logs every traced kernel call.

#include "apu/xma.h"
#include "host/input.h"
#include "host/window.h"
#include "kernel/kernel.h"
#include "loader.h"

#include <filesystem>
#include <windows.h>
#include <dbghelp.h>

PPC_EXTERN_FUNC(_xstart);

namespace fs = std::filesystem;

namespace
{
    // Prints the host stack with symbol names. Recompiled functions are named after their guest
    // address (__imp__sub_XXXXXXXX), so this is also the guest call stack.
    void PrintStack(CONTEXT* context)
    {
        HANDLE process = GetCurrentProcess();
        HANDLE thread = GetCurrentThread();
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = context->Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context->Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context->Rsp;
        frame.AddrStack.Mode = AddrModeFlat;

        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        for (int i = 0; i < 40; i++)
        {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, context, nullptr,
                    SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || frame.AddrPC.Offset == 0)
                break;
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol))
                Log("  #%-2d %s+0x%llX", i, symbol->Name, (unsigned long long)displacement);
            else
                Log("  #%-2d %016llX", i, (unsigned long long)frame.AddrPC.Offset);
        }
    }

    LONG WINAPI CrashHandler(EXCEPTION_POINTERS* info)
    {
        auto* record = info->ExceptionRecord;
        auto* thread = kernel::CurrentThread();
        Log("crash: exception %08lX at %p (guest thread %u%s%s)", record->ExceptionCode, record->ExceptionAddress,
            thread ? thread->id : 0, thread && !thread->name.empty() ? ", " : "", thread ? thread->name.c_str() : "");
        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        {
            auto* address = reinterpret_cast<uint8_t*>(record->ExceptionInformation[1]);
            const char* access = record->ExceptionInformation[0] == 1 ? "write" : "read";
            if (address >= mem::g_base && address < mem::g_base + mem::kSize * 2)
                Log("crash: %s at guest address %08llX", access, (unsigned long long)(address - mem::g_base));
            else
                Log("crash: %s at host address %p", access, address);
        }
        PrintStack(info->ContextRecord);
        kernel::PrintImportSummary();
        std::_Exit(1);
    }

    fs::path ExecutableDirectory()
    {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return fs::path(path).parent_path();
    }

    // Host thread of the guest main thread. The host main thread runs the window.
    DWORD WINAPI GuestMain(void*)
    {
        kernel::StartKernelClock();
        Log("time base: %llu Hz", (unsigned long long)kernel::TimeBaseFrequency());
        Log("running _xstart");
        kernel::RunMainThread(_xstart, kernel::g_module.stackSize);
        Log("_xstart returned");
        host::RequestQuit();
        return 0;
    }
}

int main(int argc, char** argv)
{
    g_trace = getenv("ROAN_TRACE") != nullptr;

    fs::path exeDir = ExecutableDirectory();
    fs::path gameDir = argc > 1 ? fs::path(argv[1]) : exeDir / ".." / ".." / "game";
    std::error_code error;
    gameDir = fs::weakly_canonical(gameDir, error);
    if (!fs::exists(gameDir / "default.xex"))
    {
        Log("error: default.xex not found in %s (pass the game folder as the first argument)", gameDir.string().c_str());
        return 1;
    }

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    SetUnhandledExceptionFilter(CrashHandler);

    if (!mem::Init())
    {
        Log("error: cannot reserve the 4 GB guest address space");
        return 1;
    }

    kernel::SetPaths(gameDir.string(), (exeDir / "userdata").string());
    if (!LoadExecutable(gameDir / "default.xex"))
        return 1;
    // XAudioInitialize reads the XMA context base early in the title's startup.
    apu::InitializeXma();

    if (!host::CreateMainWindow("roan-xenon", 1280, 720))
        return 1;

    // Recompiled code recurses deeply on the host stack: same reservation as the other guest threads.
    HANDLE guest = CreateThread(nullptr, 16 * 1024 * 1024, GuestMain, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (guest == nullptr)
    {
        Log("error: cannot create the guest main thread");
        return 1;
    }
    CloseHandle(guest);

    if (host::RunEventLoop())
        Log("window closed");
    host::StopInput();
    kernel::PrintImportSummary();
    // Plain exit: guest threads are still running, so no static destructors.
    std::_Exit(0);
}
