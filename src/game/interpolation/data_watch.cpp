// The data watch (data_watch.h): a hardware data breakpoint on the view's display counter.

#include "game/interpolation/data_watch.h"

#include "game/addresses.h"
#include "game/interpolation/diagnostics.h"
#include "host/window.h"
#include "kernel/kernel.h"
#include "log.h"
#include "memory.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

namespace game::interpolation::watch
{
    namespace
    {
        constexpr uint32_t kSteps = 8;          // main steps watched per F8 press
        constexpr uint32_t kCounterOffset = jade::view::kRenderingCounter;
        constexpr int kStackDepth = 10;

        // One host instruction that touched the word (the breakpoint traps after it).
        struct Site
        {
            std::string thread;
            uint64_t readsInBlock = 0, writesInBlock = 0;
            uint64_t readsOutside = 0, writesOutside = 0;
            uint32_t firstStep = 0;
            uint32_t firstValue = 0;
            uint64_t stack[kStackDepth] = {};
        };

        int g_enabled = -1;
        uint32_t g_presses = 0;
        uint32_t g_report = 0;
        uint32_t g_stepsLeft = 0; // main thread
        bool g_armed = false;     // main thread: install at the next delivery
        uint32_t g_watched = 0;   // guest address of the watched word, 0 when none (main thread)
        uint32_t g_fixedAddress = 0; // ROAN_DATA_WATCH_ADDRESS (hex), else the view's counter

        // Shared with the exception handler, which runs on whichever thread touched the word.
        std::atomic<uint8_t*> g_host{ nullptr };
        std::atomic<bool> g_inBlock{ false };
        std::atomic<uint32_t> g_step{ 0 };
        std::mutex g_mutex; // g_sites, g_last and dbghelp
        std::unordered_map<uint64_t, Site> g_sites;
        uint32_t g_last = 0;
        // Set while the handler runs on this thread: its own read of the word traps again.
        thread_local bool t_inHandler = false;

        uint32_t LoadWord(const uint8_t* host)
        {
            uint32_t value;
            std::memcpy(&value, host, 4);
            return _byteswap_ulong(value);
        }

        void CaptureStack(const CONTEXT& faulting, uint64_t* stack)
        {
            CONTEXT context = faulting;
            STACKFRAME64 frame{};
            frame.AddrPC.Offset = context.Rip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = context.Rbp;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = context.Rsp;
            frame.AddrStack.Mode = AddrModeFlat;
            for (int i = 0; i < kStackDepth; i++)
            {
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(), &frame, &context,
                        nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || frame.AddrPC.Offset == 0)
                    break;
                stack[i] = frame.AddrPC.Offset;
            }
        }

        // Counts one access at the instruction before context.Rip. A value different from the last one
        // seen is a write.
        void Record(const CONTEXT& context, uint32_t value, bool inBlock)
        {
            std::lock_guard lock(g_mutex);
            bool write = value != g_last;
            g_last = value;
            auto [it, added] = g_sites.try_emplace(context.Rip);
            Site& site = it->second;
            if (added)
            {
                auto* thread = kernel::CurrentThread();
                site.thread = thread == nullptr ? "host" : thread->name.empty() ? std::to_string(thread->id) : thread->name;
                site.firstStep = g_step.load();
                site.firstValue = value;
                CaptureStack(context, site.stack);
            }
            uint64_t& count = inBlock ? (write ? site.writesInBlock : site.readsInBlock)
                                      : (write ? site.writesOutside : site.readsOutside);
            count++;
        }

        // The data breakpoint's single-step trap. Counts the access and continues; any other exception
        // goes on to the next handler.
        LONG CALLBACK OnTrap(EXCEPTION_POINTERS* info)
        {
            if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
                return EXCEPTION_CONTINUE_SEARCH;
            CONTEXT* context = info->ContextRecord;
            if ((context->Dr6 & 1) == 0)
                return EXCEPTION_CONTINUE_SEARCH;
            context->Dr6 = 0;
            uint8_t* host = g_host.load();
            // A trap that was pending when the watch ended, or the handler's own read below.
            if (host == nullptr || t_inHandler)
                return EXCEPTION_CONTINUE_EXECUTION;

            t_inHandler = true;
            Record(*context, LoadWord(host), g_inBlock.load());
            t_inHandler = false;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // Sets (address != nullptr) or clears debug register 0 on every thread of the process. Runs on a
        // helper thread, since a thread cannot change its own debug registers; each thread is suspended
        // only while its context is set, and nothing is allocated meanwhile.
        void SetBreakpoint(void* address)
        {
            std::thread helper([address] {
                DWORD self = GetCurrentThreadId();
                DWORD process = GetCurrentProcessId();
                std::vector<DWORD> threads;
                HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                if (snapshot == INVALID_HANDLE_VALUE)
                    return;
                THREADENTRY32 entry{ sizeof(entry) };
                for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
                {
                    if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self)
                        threads.push_back(entry.th32ThreadID);
                }
                CloseHandle(snapshot);

                uint32_t done = 0;
                for (DWORD id : threads)
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
                            context.Dr0 = reinterpret_cast<DWORD64>(address);
                            // L0 (bit 0), R/W0 = 11 (reads and writes, bits 16-17), LEN0 = 11 (4 bytes, bits 18-19).
                            context.Dr7 = address != nullptr ? (context.Dr7 & ~DWORD64(0xF0003)) | 0xF0001
                                                             : context.Dr7 & ~DWORD64(0xF0003);
                            context.Dr6 = 0;
                            if (SetThreadContext(thread, &context))
                                done++;
                        }
                        ResumeThread(thread);
                    }
                    CloseHandle(thread);
                }
                Log("watch: %s the breakpoint on %u of %zu threads", address != nullptr ? "set" : "cleared", done,
                    threads.size());
            });
            helper.join();
        }

        std::string Symbol(uint64_t address)
        {
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            char text[320];
            if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol))
                std::snprintf(text, sizeof(text), "%s+0x%llX", symbol->Name, (unsigned long long)displacement);
            else
                std::snprintf(text, sizeof(text), "%016llX", (unsigned long long)address);
            return text;
        }

        void WriteReport()
        {
            std::lock_guard lock(g_mutex);
            std::vector<std::pair<uint64_t, const Site*>> sites;
            for (auto& [rip, site] : g_sites)
                sites.emplace_back(rip, &site);
            auto total = [](const Site* s) { return s->readsInBlock + s->writesInBlock + s->readsOutside + s->writesOutside; };
            std::sort(sites.begin(), sites.end(), [&](auto& a, auto& b) { return total(a.second) > total(b.second); });

            char name[64];
            std::snprintf(name, sizeof(name), "data-watch-%u.txt", g_report);
            std::FILE* file = OpenDiagnosticsFile(name);
            Log("watch %u: guest %08X, %zu instructions touched it", g_report, g_watched, sites.size());
            for (auto& [rip, site] : sites)
            {
                std::string where = Symbol(rip);
                Log("watch %u:   %-40s %-16s block r%llu w%llu, outside r%llu w%llu", g_report, where.c_str(),
                    site->thread.c_str(), site->readsInBlock, site->writesInBlock, site->readsOutside, site->writesOutside);
                if (file == nullptr)
                    continue;
                std::fprintf(file, "%s  thread %s  view block: reads %llu writes %llu  outside: reads %llu writes %llu"
                                   "  first: step %u value %08X\n",
                    where.c_str(), site->thread.c_str(), site->readsInBlock, site->writesInBlock, site->readsOutside,
                    site->writesOutside, site->firstStep, site->firstValue);
                for (int i = 0; i < kStackDepth && site->stack[i] != 0; i++)
                    std::fprintf(file, "    #%d %s\n", i, Symbol(site->stack[i]).c_str());
            }
            if (file != nullptr)
                std::fclose(file);
            g_sites.clear();
        }
    }

    bool Enabled()
    {
        if (g_enabled < 0)
        {
            g_enabled = EnvironmentFlag("ROAN_DATA_WATCH") ? 1 : 0;
            if (g_enabled == 1)
            {
                if (const char* address = std::getenv("ROAN_DATA_WATCH_ADDRESS"))
                    g_fixedAddress = uint32_t(std::strtoul(address, nullptr, 16)) & ~3u;
                AddVectoredExceptionHandler(1, OnTrap);
                g_presses = host::DiagnosticKeyPresses();
                if (g_fixedAddress != 0)
                    Log("watch: armed, press F8 in the game window to watch %08X for %u main steps", g_fixedAddress, kSteps);
                else
                    Log("watch: armed, press F8 in the game window to watch view+0x%X for %u main steps", kCounterOffset, kSteps);
            }
        }
        return g_enabled == 1;
    }

    void MainStepStart()
    {
        uint32_t presses = host::DiagnosticKeyPresses();
        bool pressed = presses != g_presses;
        g_presses = presses;
        if (pressed && !g_armed && g_watched == 0)
        {
            g_armed = true;
            g_report++;
            Log("watch %u: watching from the next delivery", g_report);
        }
        g_step.fetch_add(1);
    }

    void ViewBlockStart()
    {
        g_inBlock.store(true);
    }

    void ViewBlockEnd(uint32_t view)
    {
        g_inBlock.store(false);
        if (!g_armed)
            return;
        g_armed = false;
        g_watched = g_fixedAddress != 0 ? g_fixedAddress : view + kCounterOffset;
        uint8_t* host = mem::Ptr(g_watched);
        {
            std::lock_guard lock(g_mutex);
            g_last = LoadWord(host);
        }
        g_host.store(host);
        g_stepsLeft = kSteps + 1; // the rest of this main step, then kSteps whole ones
        SetBreakpoint(host);
    }

    void MainStepEnd()
    {
        if (g_watched == 0 || --g_stepsLeft != 0)
            return;
        SetBreakpoint(nullptr);
        g_host.store(nullptr);
        WriteReport();
        g_watched = 0;
    }
}
