// Host debugging on POSIX: symbols, stack walks, the crash handler, data breakpoints (debug.h).

#include "host/debug.h"

#include "host/posix/signal_stack.h"
#include "log.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <ucontext.h>
#include <unistd.h>

namespace host
{
    namespace
    {
        // The signals of a crash: what Windows reports as an exception nothing handles.
        constexpr int kCrashSignals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE };
        // Frames of the handler and of the system's signal return above the crashed one, and the most
        // a walk reads.
        constexpr int kHandlerFrames = 16;
        constexpr int kWalkLimit = 128;
        // The page fault error code's bit for a write (x86-64).
        constexpr greg_t kPageFaultWrite = 2;

        CrashHandler g_crashHandler = nullptr;
        std::atomic<bool> g_crashing{ false };

        // The stack of the calling thread from the frame at instruction on: the walk starts in the
        // caller, and a signal frame (the crashed or trapped thread's registers) is unwound like any
        // other, so the frames above it are skipped. All of them when instruction is not on the stack.
        int WalkFrom(uint64_t instruction, uint64_t* frames, int depth)
        {
            void* addresses[kWalkLimit];
            int count = backtrace(addresses, std::min(depth + kHandlerFrames, kWalkLimit));
            int first = 0;
            while (first < count && reinterpret_cast<uint64_t>(addresses[first]) != instruction)
                first++;
            if (first == count)
                first = 0;
            int written = 0;
            for (int i = first; i < count && written < depth; i++)
                frames[written++] = reinterpret_cast<uint64_t>(addresses[i]);
            return written;
        }

        void OnCrash(int signal, siginfo_t* info, void* system)
        {
            auto* context = static_cast<ucontext_t*>(system);
            Crash crash;
            std::snprintf(crash.description, sizeof(crash.description), "signal %d (SIG%s)", signal,
                sigabbrev_np(signal));
            crash.address = reinterpret_cast<const void*>(context->uc_mcontext.gregs[REG_RIP]);
            if (signal == SIGSEGV || signal == SIGBUS)
            {
                crash.memoryFault = true;
                crash.data = info->si_addr;
                crash.write = (context->uc_mcontext.gregs[REG_ERR] & kPageFaultWrite) != 0;
            }
            crash.system = context;
            // One report: a thread that crashes while another reports waits for the process to end.
            if (g_crashing.exchange(true))
            {
                for (;;)
                    pause();
            }
            g_crashHandler(crash);
            // The handler returned: the default action, at the instruction that faults again.
            std::signal(signal, SIG_DFL);
        }
    }

    void InstallCrashHandler(CrashHandler handler)
    {
        g_crashHandler = handler;
        // backtrace loads the unwinder on its first call: not inside a crash.
        void* warm[1];
        backtrace(warm, 1);
        // The calling thread's alternate stack, for its life (the main thread's); the threads of
        // StartDetachedThread make their own.
        static posix::SignalStack mainStack;
        struct sigaction action = {};
        action.sa_sigaction = OnCrash;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        for (int signal : kCrashSignals)
            sigaction(signal, &action, nullptr);
    }

    int WalkStack(const MachineContext& context, uint64_t* frames, int depth)
    {
        return WalkFrom(context.instruction, frames, depth);
    }

    int WalkCrashStack(const Crash& crash, uint64_t* frames, int depth)
    {
        return WalkFrom(reinterpret_cast<uint64_t>(crash.address), frames, depth);
    }

    void FormatSymbol(uint64_t address, char* text, size_t size)
    {
        // The executable exports the recompiled functions and their hooks (CMakeLists.txt), so those
        // have names here; any other address of a module is given as the module and its offset, which
        // addr2line resolves on the build.
        Dl_info info;
        if (dladdr(reinterpret_cast<void*>(address), &info) == 0)
        {
            std::snprintf(text, size, "%016llX", (unsigned long long)address);
            return;
        }
        if (info.dli_sname == nullptr)
        {
            const char* module = info.dli_fname != nullptr ? std::strrchr(info.dli_fname, '/') : nullptr;
            std::snprintf(text, size, "%s+0x%llX", module != nullptr ? module + 1 : "?",
                (unsigned long long)(address - reinterpret_cast<uint64_t>(info.dli_fbase)));
            return;
        }
        int status = 0;
        char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
        std::snprintf(text, size, "%s+0x%llX", status == 0 ? demangled : info.dli_sname,
            (unsigned long long)(address - reinterpret_cast<uint64_t>(info.dli_saddr)));
        std::free(demangled);
    }

    void InstallDataBreakpointHandler(DataBreakpointHandler)
    {
    }

    bool SetDataBreakpoints(const std::array<void*, kDataBreakpoints>&, uint32_t& set, uint32_t& threads)
    {
        set = 0;
        threads = 0;
        LOG_ONCE("error: data breakpoints are not implemented on this system");
        return false;
    }
}
