// Host debugging on Windows: symbols, stack walks, the crash handler, data breakpoints (debug.h).

#include "host/debug.h"

#include <cstdio>
#include <thread>
#include <vector>
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

namespace host
{
    namespace
    {
        CrashHandler g_crashHandler = nullptr;
        DataBreakpointHandler g_dataBreakpointHandler = nullptr;

        // Walks the stack of context, which the walk changes.
        int Walk(CONTEXT& context, uint64_t* frames, int depth)
        {
            STACKFRAME64 frame{};
            frame.AddrPC.Offset = context.Rip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = context.Rbp;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = context.Rsp;
            frame.AddrStack.Mode = AddrModeFlat;
            int count = 0;
            while (count < depth)
            {
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(), &frame, &context,
                        nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || frame.AddrPC.Offset == 0)
                    break;
                frames[count++] = frame.AddrPC.Offset;
            }
            return count;
        }

        // The unhandled exception filter.
        LONG WINAPI OnCrash(EXCEPTION_POINTERS* info)
        {
            auto* record = info->ExceptionRecord;
            Crash crash;
            std::snprintf(crash.description, sizeof(crash.description), "exception %08lX", record->ExceptionCode);
            crash.address = record->ExceptionAddress;
            if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
            {
                crash.memoryFault = true;
                crash.data = reinterpret_cast<const void*>(record->ExceptionInformation[1]);
                crash.write = record->ExceptionInformation[0] == 1;
            }
            crash.system = info->ContextRecord;
            g_crashHandler(crash);
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // The data breakpoints' single-step trap; any other exception goes on to the next handler.
        LONG CALLBACK OnTrap(EXCEPTION_POINTERS* info)
        {
            if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
                return EXCEPTION_CONTINUE_SEARCH;
            CONTEXT* context = info->ContextRecord;
            DWORD64 hits = context->Dr6 & 0xF;
            if (hits == 0)
                return EXCEPTION_CONTINUE_SEARCH;
            context->Dr6 = 0;
            g_dataBreakpointHandler(uint32_t(hits), MachineContext{ context->Rip, context });
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    void InstallCrashHandler(CrashHandler handler)
    {
        g_crashHandler = handler;
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        SetUnhandledExceptionFilter(OnCrash);
    }

    int WalkStack(const MachineContext& context, uint64_t* frames, int depth)
    {
        CONTEXT copy = *static_cast<const CONTEXT*>(context.system);
        return Walk(copy, frames, depth);
    }

    int WalkCrashStack(const Crash& crash, uint64_t* frames, int depth)
    {
        return Walk(*static_cast<CONTEXT*>(crash.system), frames, depth);
    }

    void FormatSymbol(uint64_t address, char* text, size_t size)
    {
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol))
            std::snprintf(text, size, "%s+0x%llX", symbol->Name, (unsigned long long)displacement);
        else
            std::snprintf(text, size, "%016llX", (unsigned long long)address);
    }

    void InstallDataBreakpointHandler(DataBreakpointHandler handler)
    {
        g_dataBreakpointHandler = handler;
        AddVectoredExceptionHandler(1, OnTrap);
    }

    bool SetDataBreakpoints(const std::array<void*, kDataBreakpoints>& addresses, uint32_t& set, uint32_t& threads)
    {
        bool listed = false;
        set = 0;
        threads = 0;
        std::thread helper([&] {
            DWORD self = GetCurrentThreadId();
            DWORD process = GetCurrentProcessId();
            std::vector<DWORD> ids;
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
                return;
            THREADENTRY32 entry{ sizeof(entry) };
            for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
            {
                if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self)
                    ids.push_back(entry.th32ThreadID);
            }
            CloseHandle(snapshot);
            listed = true;
            threads = uint32_t(ids.size());

            // Per breakpoint i: Li (bit 2i), R/Wi = 11 (reads and writes, bits 16+4i..17+4i), LENi = 11 (4
            // bytes, bits 18+4i..19+4i).
            DWORD64 mask = 0, bits = 0;
            for (uint32_t i = 0; i < kDataBreakpoints; i++)
            {
                mask |= (DWORD64(3) << (2 * i)) | (DWORD64(0xF) << (16 + 4 * i));
                if (addresses[i] != nullptr)
                    bits |= (DWORD64(1) << (2 * i)) | (DWORD64(0xF) << (16 + 4 * i));
            }
            // Nothing is allocated while a thread is suspended: it may hold the heap's lock.
            for (DWORD id : ids)
            {
                HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, id);
                if (thread == nullptr)
                    continue;
                if (SuspendThread(thread) != DWORD(-1))
                {
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    if (GetThreadContext(thread, &context))
                    {
                        context.Dr0 = reinterpret_cast<DWORD64>(addresses[0]);
                        context.Dr1 = reinterpret_cast<DWORD64>(addresses[1]);
                        context.Dr2 = reinterpret_cast<DWORD64>(addresses[2]);
                        context.Dr3 = reinterpret_cast<DWORD64>(addresses[3]);
                        context.Dr7 = (context.Dr7 & ~mask) | bits;
                        context.Dr6 = 0;
                        if (SetThreadContext(thread, &context))
                            set++;
                    }
                    ResumeThread(thread);
                }
                CloseHandle(thread);
            }
        });
        helper.join();
        return listed;
    }
}
