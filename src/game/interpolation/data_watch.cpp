// The data watch (data_watch.h): hardware data breakpoints on up to four guest words.

#include "game/interpolation/data_watch.h"

#include "game/addresses.h"
#include "game/interpolation/diagnostics.h"
#include "game/interpolation/world.h"
#include "host/window.h"
#include "kernel/kernel.h"
#include "log.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

namespace game::interpolation::watch
{
    namespace
    {
        constexpr uint32_t kSteps = 8;          // main steps watched per F8 press
        constexpr uint32_t kSlots = 4;          // the debug registers 0 to 3
        constexpr uint32_t kCounterOffset = jade::view::kRenderingCounter;
        constexpr int kStackDepth = 10;

        // One host instruction that touched a watched word (the breakpoint traps after it).
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
        bool g_watching = false;  // main thread
        // ROAN_DATA_WATCH_ADDRESS (up to kSlots hex addresses, comma-separated), else the view's counter.
        std::vector<uint32_t> g_fixedAddresses;
        // ROAN_DATA_WATCH_OBJECT (hex offsets): words of the first object with dynamics the display
        // rendered in the view block the watch starts after.
        std::vector<uint32_t> g_objectOffsets;
        uint32_t g_actor = 0; // main thread: that object in the current view block
        std::array<uint32_t, kSlots> g_watched{}; // guest addresses of the watched words, 0 when unused

        // Shared with the exception handler, which runs on whichever thread touched a word.
        std::array<std::atomic<uint8_t*>, kSlots> g_host{};
        std::atomic<bool> g_inBlock{ false };
        std::atomic<uint32_t> g_step{ 0 };
        std::mutex g_mutex; // g_sites, g_last and dbghelp
        std::map<std::pair<uint32_t, uint64_t>, Site> g_sites; // by slot and instruction
        std::array<uint32_t, kSlots> g_last{};
        // Set while the handler runs on this thread: its own read of a word traps again.
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

        // Counts one access of slot's word at the instruction before context.Rip. A value different
        // from the last one seen is a write.
        void Record(uint32_t slot, const CONTEXT& context, uint32_t value, bool inBlock)
        {
            std::lock_guard lock(g_mutex);
            bool write = value != g_last[slot];
            g_last[slot] = value;
            auto [it, added] = g_sites.try_emplace({ slot, context.Rip });
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

        // The data breakpoints' single-step trap. Counts the access and continues; any other exception
        // goes on to the next handler.
        LONG CALLBACK OnTrap(EXCEPTION_POINTERS* info)
        {
            if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
                return EXCEPTION_CONTINUE_SEARCH;
            CONTEXT* context = info->ContextRecord;
            DWORD64 hits = context->Dr6 & 0xF;
            if (hits == 0)
                return EXCEPTION_CONTINUE_SEARCH;
            context->Dr6 = 0;
            // A trap that was pending when the watch ended, or the handler's own read below.
            if (t_inHandler)
                return EXCEPTION_CONTINUE_EXECUTION;
            t_inHandler = true;
            for (uint32_t slot = 0; slot < kSlots; slot++)
            {
                uint8_t* host = g_host[slot].load();
                if ((hits & (1ull << slot)) != 0 && host != nullptr)
                    Record(slot, *context, LoadWord(host), g_inBlock.load());
            }
            t_inHandler = false;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // Sets (an address not null) or clears debug registers 0 to 3 on every thread of the process. Runs
        // on a helper thread, since a thread cannot change its own debug registers; each thread is
        // suspended only while its context is set, and nothing is allocated meanwhile.
        void SetBreakpoints(const std::array<void*, kSlots>& addresses)
        {
            std::thread helper([addresses] {
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

                // Per slot i: Li (bit 2i), R/Wi = 11 (reads and writes, bits 16+4i..17+4i), LENi = 11 (4
                // bytes, bits 18+4i..19+4i).
                DWORD64 mask = 0, bits = 0;
                for (uint32_t i = 0; i < kSlots; i++)
                {
                    mask |= (DWORD64(3) << (2 * i)) | (DWORD64(0xF) << (16 + 4 * i));
                    if (addresses[i] != nullptr)
                        bits |= (DWORD64(1) << (2 * i)) | (DWORD64(0xF) << (16 + 4 * i));
                }
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
                            context.Dr0 = reinterpret_cast<DWORD64>(addresses[0]);
                            context.Dr1 = reinterpret_cast<DWORD64>(addresses[1]);
                            context.Dr2 = reinterpret_cast<DWORD64>(addresses[2]);
                            context.Dr3 = reinterpret_cast<DWORD64>(addresses[3]);
                            context.Dr7 = (context.Dr7 & ~mask) | bits;
                            context.Dr6 = 0;
                            if (SetThreadContext(thread, &context))
                                done++;
                        }
                        ResumeThread(thread);
                    }
                    CloseHandle(thread);
                }
                Log("watch: %s the breakpoints on %u of %zu threads", bits != 0 ? "set" : "cleared", done,
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
            auto total = [](const Site& s) { return s.readsInBlock + s.writesInBlock + s.readsOutside + s.writesOutside; };
            char name[64];
            std::snprintf(name, sizeof(name), "data-watch-%u.txt", g_report);
            std::FILE* file = OpenDiagnosticsFile(name);
            for (uint32_t slot = 0; slot < kSlots; slot++)
            {
                if (g_watched[slot] == 0)
                    continue;
                std::vector<std::pair<uint64_t, const Site*>> sites;
                for (auto& [key, site] : g_sites)
                    if (key.first == slot)
                        sites.emplace_back(key.second, &site);
                std::sort(sites.begin(), sites.end(), [&](auto& a, auto& b) { return total(*a.second) > total(*b.second); });
                Log("watch %u: guest %08X, %zu instructions touched it", g_report, g_watched[slot], sites.size());
                if (file != nullptr)
                    std::fprintf(file, "== guest %08X\n", g_watched[slot]);
                for (auto& [rip, site] : sites)
                {
                    std::string where = Symbol(rip);
                    Log("watch %u:   %08X %-40s %-16s block r%llu w%llu, outside r%llu w%llu", g_report, g_watched[slot],
                        where.c_str(), site->thread.c_str(), site->readsInBlock, site->writesInBlock, site->readsOutside,
                        site->writesOutside);
                    if (file == nullptr)
                        continue;
                    std::fprintf(file, "%s  thread %s  view block: reads %llu writes %llu  outside: reads %llu writes %llu"
                                       "  first: step %u value %08X\n",
                        where.c_str(), site->thread.c_str(), site->readsInBlock, site->writesInBlock, site->readsOutside,
                        site->writesOutside, site->firstStep, site->firstValue);
                    for (int i = 0; i < kStackDepth && site->stack[i] != 0; i++)
                        std::fprintf(file, "    #%d %s\n", i, Symbol(site->stack[i]).c_str());
                }
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
                auto parse = [](const char* name, std::vector<uint32_t>& out, bool zero) {
                    const char* list = std::getenv(name);
                    for (const char* p = list; p != nullptr && *p != 0 && out.size() < kSlots;)
                    {
                        char* end = nullptr;
                        uint32_t value = uint32_t(std::strtoul(p, &end, 16)) & ~3u;
                        if (end == p)
                            break;
                        if (value != 0 || zero)
                            out.push_back(value);
                        p = *end == ',' ? end + 1 : end;
                    }
                };
                parse("ROAN_DATA_WATCH_ADDRESS", g_fixedAddresses, false);
                parse("ROAN_DATA_WATCH_OBJECT", g_objectOffsets, true);
                if (g_objectOffsets.size() > kSlots - std::min<size_t>(g_fixedAddresses.size(), kSlots))
                    g_objectOffsets.resize(kSlots - std::min<size_t>(g_fixedAddresses.size(), kSlots));
                AddVectoredExceptionHandler(1, OnTrap);
                g_presses = host::DiagnosticKeyPresses();
                if (!g_fixedAddresses.empty() || !g_objectOffsets.empty())
                    Log("watch: armed, press F8 in the game window to watch %zu words for %u main steps",
                        g_fixedAddresses.size() + g_objectOffsets.size(), kSteps);
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
        if (pressed && !g_armed && !g_watching)
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
        g_actor = 0;
    }

    void NoteRendered(uint32_t object)
    {
        if (g_actor == 0 && HasDynamics(object))
            g_actor = object;
    }

    void ViewBlockEnd(uint32_t view, bool sectorSwitch)
    {
        g_inBlock.store(false);
        if (!g_armed)
            return;
        // No actor in this view block, or the delivery's sector switch may have destroyed it: the next.
        if (!g_objectOffsets.empty() && (g_actor == 0 || sectorSwitch))
            return;
        g_armed = false;
        g_watched = {};
        if (g_fixedAddresses.empty() && g_objectOffsets.empty())
            g_watched[0] = view + kCounterOffset;
        std::copy(g_fixedAddresses.begin(), g_fixedAddresses.end(), g_watched.begin());
        for (size_t i = 0; i < g_objectOffsets.size(); i++)
            g_watched[g_fixedAddresses.size() + i] = g_actor + g_objectOffsets[i];
        if (g_actor != 0)
            Log("watch %u: object %08X", g_report, g_actor);
        std::array<void*, kSlots> hosts{};
        {
            std::lock_guard lock(g_mutex);
            for (uint32_t slot = 0; slot < kSlots; slot++)
            {
                if (g_watched[slot] == 0)
                    continue;
                hosts[slot] = mem::Ptr(g_watched[slot]);
                g_last[slot] = LoadWord(static_cast<uint8_t*>(hosts[slot]));
            }
        }
        for (uint32_t slot = 0; slot < kSlots; slot++)
            g_host[slot].store(static_cast<uint8_t*>(hosts[slot]));
        g_stepsLeft = kSteps + 1; // the rest of this main step, then kSteps whole ones
        g_watching = true;
        SetBreakpoints(hosts);
    }

    void MainStepEnd()
    {
        if (!g_watching || --g_stepsLeft != 0)
            return;
        SetBreakpoints({});
        for (auto& host : g_host)
            host.store(nullptr);
        WriteReport();
        g_watching = false;
    }
}
