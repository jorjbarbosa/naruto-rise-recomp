// An out-of-process sampling profiler of roan-xenon's threads on Windows (scripts/profile.ps1).
//
// Every round it reads the state of the target's threads (one NtQuerySystemInformation call), then
// suspends each thread that is not waiting, copies its registers and stack, resumes it and unwinds the
// copy with dbghelp against the target's symbols (roan-xenon.pdb: recompiled guest functions show as
// sub_XXXXXXXX, a hook's original as __imp__sub_XXXXXXXX, counted as one). A sample counts as self time
// for the function it stopped in and as inclusive time for every function on its stack; a sample
// stopped in a wait call is dropped. At the end each thread's CPU time (GetThreadTimes) is shared among
// its functions in proportion to their samples. The symbol search path is the target's directory only,
// so no symbol server is asked.
//
// Short functions called very often come out larger than they are, and a function that waits on
// memory hides its cache misses: rank with this, then time a candidate directly before replacing it.

#include <windows.h>

#include <dbghelp.h>
#include <timeapi.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    constexpr size_t kMaxStackCopy = 512 * 1024;
    constexpr int kMaxFrames = 256;
    // How often new threads and newly loaded modules are looked for.
    constexpr uint64_t kRefreshMs = 500;
    constexpr uint64_t kMinBusySamples = 20;

    // The system calls a waiting thread sits in (ntdll exports each also as Zw...): a sample stopped in
    // one is not CPU time.
    constexpr const char* kWaitCalls[] = {
        "WaitForSingleObject", "WaitForMultipleObjects", "WaitForAlertByThreadId", "DelayExecution",
        "RemoveIoCompletion", "RemoveIoCompletionEx", "WaitForWorkViaWorkerFactory", "WaitForKeyedEvent",
        "SignalAndWaitForSingleObject", "UserMsgWaitForMultipleObjectsEx", "UserGetMessage", "UserWaitMessage",
    };

    struct Options
    {
        DWORD pid = 0;
        double seconds = 20.0;
        int rate = 200; // Sampling rounds a second.
        int top = 25;
        double fps = 0.0;  // The target's frame rate during the run, to report ms a frame.
        std::string names; // A file of address<TAB>name lines (tools/dev/ida/names.tsv).
    };

    struct Counts
    {
        uint64_t self = 0;
        uint64_t inclusive = 0;
    };

    struct Thread
    {
        DWORD id = 0;
        HANDLE handle = nullptr;
        std::string label;
        uint64_t stackBase = 0;
        uint64_t startCpu = 0; // CPU time when first seen, in 100 ns units.
        uint64_t endCpu = 0;   // CPU time when it exited, 0 while it runs.
        uint64_t busy = 0;
        uint64_t waiting = 0;
        std::unordered_map<uint32_t, Counts> functions; // By name id.
        // The guest functions, from the outermost, on the stack of every sample so far.
        std::vector<uint32_t> guestPath;
        bool sampledPath = false;
    };

    struct FrameKey
    {
        uint64_t pc;
        uint32_t inlineContext;
        bool operator==(const FrameKey& other) const { return pc == other.pc && inlineContext == other.inlineContext; }
    };

    struct FrameKeyHash
    {
        size_t operator()(const FrameKey& key) const { return std::hash<uint64_t>()(key.pc * 31 + key.inlineContext); }
    };

    HANDLE g_process = nullptr;
    uint64_t g_mainModule = 0;
    std::vector<uint8_t> g_stack;
    uint64_t g_stackAddress = 0;
    size_t g_stackSize = 0;
    std::vector<std::string> g_names;
    std::vector<bool> g_isWait;
    std::vector<bool> g_isGuest; // Whether a name id is a recompiled function (sub_XXXXXXXX).
    std::unordered_map<std::string, uint32_t> g_nameIds;
    std::unordered_map<FrameKey, uint32_t, FrameKeyHash> g_frameNames;
    std::unordered_map<uint64_t, std::string> g_moduleNames;
    std::unordered_map<std::string, std::string> g_guestNames; // From "sub_XXXXXXXX" to its curated name.
    // Set by Ctrl+C: the loop stops after its round, so no thread of the target stays suspended.
    std::atomic<bool> g_stop{ false };

    using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

    struct ThreadBasicInformation
    {
        LONG exitStatus;
        PVOID teb;
        HANDLE clientProcess;
        HANDLE clientThread;
        ULONG_PTR affinityMask;
        LONG priority;
        LONG basePriority;
    };

    uint64_t FileTimeTicks(FILETIME time)
    {
        return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    }

    uint64_t ThreadCpu(HANDLE thread)
    {
        FILETIME created, exited, kernelTime, userTime;
        if (!GetThreadTimes(thread, &created, &exited, &kernelTime, &userTime))
            return 0;
        return FileTimeTicks(kernelTime) + FileTimeTicks(userTime);
    }

    std::string Narrow(const wchar_t* text)
    {
        int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        std::string result(size > 0 ? size - 1 : 0, '\0');
        if (size > 1)
            WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
        return result;
    }

    // The top of the thread's stack, from its TEB (NT_TIB::StackBase); 0 when it cannot be read.
    uint64_t StackBase(HANDLE thread)
    {
        static auto query = reinterpret_cast<NtQueryInformationThreadFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
        ThreadBasicInformation info{};
        if (!query || query(thread, 0, &info, sizeof(info), nullptr) < 0)
            return 0;
        uint64_t base = 0;
        SIZE_T read = 0;
        if (!ReadProcessMemory(g_process, static_cast<uint8_t*>(info.teb) + 8, &base, sizeof(base), &read))
            return 0;
        return base;
    }

    uint32_t NameId(const std::string& name)
    {
        auto it = g_nameIds.find(name);
        if (it != g_nameIds.end())
            return it->second;
        uint32_t id = uint32_t(g_names.size());
        g_names.push_back(name);
        g_isGuest.push_back(name.rfind("sub_", 0) == 0);
        size_t bang = name.find('!');
        std::string bare = bang == std::string::npos ? name : name.substr(bang + 1);
        if (bare.rfind("Nt", 0) == 0 || bare.rfind("Zw", 0) == 0)
            bare.erase(0, 2);
        else
            bare.clear();
        g_isWait.push_back(std::any_of(std::begin(kWaitCalls), std::end(kWaitCalls),
            [&](const char* call) { return bare == call; }));
        g_nameIds.emplace(name, id);
        return id;
    }

    const std::string& ModuleName(uint64_t base)
    {
        auto it = g_moduleNames.find(base);
        if (it != g_moduleNames.end())
            return it->second;
        IMAGEHLP_MODULE64 module{};
        module.SizeOfStruct = sizeof(module);
        std::string name = SymGetModuleInfo64(g_process, base, &module) ? module.ModuleName : "?";
        return g_moduleNames.emplace(base, name).first->second;
    }

    // The name a frame is counted under: the main module's symbols bare (a hook's original folded into
    // the hook, a guest function with its curated name), other modules' as module!symbol.
    uint32_t FrameName(uint64_t pc, uint32_t inlineContext)
    {
        FrameKey key{ pc, inlineContext };
        auto it = g_frameNames.find(key);
        if (it != g_frameNames.end())
            return it->second;

        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        std::memset(buffer, 0, sizeof(buffer));
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        DWORD64 displacement = 0;
        bool found = SymFromInlineContext(g_process, pc, inlineContext, &displacement, symbol) ||
            SymFromAddr(g_process, pc, &displacement, symbol);
        uint64_t moduleBase = SymGetModuleBase64(g_process, pc);

        std::string name = found ? symbol->Name : "?";
        if (name.rfind("__imp__sub_", 0) == 0)
            name.erase(0, 7);
        if (name.rfind("sub_", 0) == 0)
        {
            auto curated = g_guestNames.find(name);
            if (curated != g_guestNames.end())
                name += " " + curated->second;
        }
        if (moduleBase != g_mainModule)
            name = (moduleBase != 0 ? ModuleName(moduleBase) : std::string("?")) + "!" + name;

        uint32_t id = NameId(name);
        g_frameNames.emplace(key, id);
        return id;
    }

    // Serves the unwinder the copied stack, and everything else (code, unwind data) from the target.
    BOOL CALLBACK ReadMemory(HANDLE process, DWORD64 address, PVOID buffer, DWORD size, LPDWORD read)
    {
        if (address >= g_stackAddress && address + size <= g_stackAddress + g_stackSize)
        {
            std::memcpy(buffer, g_stack.data() + (address - g_stackAddress), size);
            *read = size;
            return TRUE;
        }
        SIZE_T done = 0;
        BOOL ok = ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), buffer, size, &done);
        *read = DWORD(done);
        return ok;
    }

    using NtQuerySystemInformationFn = LONG(NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
    constexpr ULONG kThreadStateWaiting = 5;           // KTHREAD_STATE::Waiting.
    constexpr LONG kStatusInfoLengthMismatch = LONG(0xC0000004);

    // The target's threads that are not waiting (running or ready to run), from one system call a round.
    // Returns false when the call fails, and then every thread is sampled.
    bool RunnableThreads(std::unordered_set<DWORD>& runnable)
    {
        static auto query = reinterpret_cast<NtQuerySystemInformationFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
        static std::vector<uint8_t> buffer(1 << 20);
        runnable.clear();
        if (!query)
            return false;
        ULONG needed = 0;
        LONG status;
        while ((status = query(SystemProcessInformation, buffer.data(), ULONG(buffer.size()), &needed)) ==
            kStatusInfoLengthMismatch)
            buffer.resize(std::max<size_t>(buffer.size() * 2, needed + 65536));
        if (status < 0)
            return false;
        DWORD pid = GetProcessId(g_process);
        for (size_t offset = 0;;)
        {
            auto* process = reinterpret_cast<SYSTEM_PROCESS_INFORMATION*>(buffer.data() + offset);
            if (DWORD(reinterpret_cast<uintptr_t>(process->UniqueProcessId)) == pid)
            {
                auto* threads = reinterpret_cast<SYSTEM_THREAD_INFORMATION*>(process + 1);
                for (ULONG i = 0; i < process->NumberOfThreads; i++)
                {
                    if (threads[i].ThreadState != kThreadStateWaiting)
                        runnable.insert(DWORD(reinterpret_cast<uintptr_t>(threads[i].ClientId.UniqueThread)));
                }
                return true;
            }
            if (process->NextEntryOffset == 0)
                return true;
            offset += process->NextEntryOffset;
        }
    }

    void Sample(Thread& thread)
    {
        if (SuspendThread(thread.handle) == DWORD(-1))
            return;
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        bool ok = GetThreadContext(thread.handle, &context) != 0;
        if (ok)
        {
            // The used stack: up to the stack base, or without it to the end of the stack's region.
            g_stackAddress = context.Rsp;
            size_t size = kMaxStackCopy;
            uint64_t end = thread.stackBase;
            MEMORY_BASIC_INFORMATION region{};
            if (end <= context.Rsp &&
                VirtualQueryEx(g_process, reinterpret_cast<LPCVOID>(context.Rsp), &region, sizeof(region)))
                end = reinterpret_cast<uint64_t>(region.BaseAddress) + region.RegionSize;
            if (end > context.Rsp)
                size = std::min<size_t>(size, size_t(end - context.Rsp));
            SIZE_T read = 0;
            ok = ReadProcessMemory(g_process, reinterpret_cast<LPCVOID>(context.Rsp), g_stack.data(), size, &read) &&
                read > 0;
            g_stackSize = read;
        }
        ResumeThread(thread.handle);
        // Without its stack the unwinder would read the running thread's: the sample is dropped.
        if (!ok)
            return;

        STACKFRAME_EX frame{};
        frame.StackFrameSize = sizeof(frame);
        frame.AddrPC.Offset = context.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        uint64_t leaf = context.Rip;

        uint32_t stack[kMaxFrames];
        int depth = 0;
        while (depth < kMaxFrames &&
            StackWalkEx(IMAGE_FILE_MACHINE_AMD64, g_process, thread.handle, &frame, &context, ReadMemory,
                SymFunctionTableAccess64, SymGetModuleBase64, nullptr, SYM_STKWALK_DEFAULT))
        {
            uint64_t pc = frame.AddrPC.Offset;
            if (pc == 0)
                break;
            // A return address points after its call: look up the call itself.
            stack[depth++] = FrameName(pc == leaf ? pc : pc - 1, frame.InlineFrameContext);
        }
        if (depth == 0)
            return;
        if (g_isWait[stack[0]])
        {
            thread.waiting++;
            return;
        }

        thread.busy++;
        size_t common = 0;
        for (int i = depth - 1; i >= 0; i--)
        {
            if (!g_isGuest[stack[i]])
                continue;
            if (!thread.sampledPath)
                thread.guestPath.push_back(stack[i]);
            else if (common < thread.guestPath.size() && thread.guestPath[common] == stack[i])
                common++;
            else
                break;
        }
        if (thread.sampledPath)
            thread.guestPath.resize(common);
        thread.sampledPath = true;
        thread.functions[stack[0]].self++;
        for (int i = 0; i < depth; i++)
        {
            bool seen = false;
            for (int j = 0; j < i && !seen; j++)
                seen = stack[j] == stack[i];
            if (!seen)
                thread.functions[stack[i]].inclusive++;
        }
    }

    // Moves the threads that exited to finished, with their final CPU time, so a new thread that
    // reuses an id is opened again; then opens the threads not seen yet.
    void RefreshThreads(std::map<DWORD, Thread>& threads, std::vector<Thread>& finished)
    {
        for (auto it = threads.begin(); it != threads.end();)
        {
            if (WaitForSingleObject(it->second.handle, 0) == WAIT_OBJECT_0)
            {
                it->second.endCpu = ThreadCpu(it->second.handle);
                finished.push_back(std::move(it->second));
                it = threads.erase(it);
            }
            else
                ++it;
        }

        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
        {
            if (entry.th32OwnerProcessID != GetProcessId(g_process) || threads.count(entry.th32ThreadID))
                continue;
            HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | SYNCHRONIZE,
                FALSE, entry.th32ThreadID);
            if (!handle)
                continue;
            Thread thread;
            thread.id = entry.th32ThreadID;
            thread.handle = handle;
            thread.stackBase = StackBase(handle);
            thread.startCpu = ThreadCpu(handle);
            PWSTR description = nullptr;
            if (SUCCEEDED(GetThreadDescription(handle, &description)) && description)
            {
                thread.label = Narrow(description);
                LocalFree(description);
            }
            threads.emplace(thread.id, std::move(thread));
        }
        CloseHandle(snapshot);
    }

    void LoadGuestNames(const std::string& path)
    {
        std::ifstream file(path);
        std::string line;
        while (std::getline(file, line))
        {
            if (line.size() < 10 || line[0] == '#')
                continue;
            size_t tab = line.find('\t');
            if (tab != 8)
                continue;
            size_t end = line.find('\t', tab + 1);
            std::string name = line.substr(tab + 1, end == std::string::npos ? std::string::npos : end - tab - 1);
            std::string address = line.substr(0, 8);
            std::transform(address.begin(), address.end(), address.begin(), ::toupper);
            if (!name.empty())
                g_guestNames["sub_" + address] = name;
        }
    }

    void PrintList(const Thread& thread, double cpuMs, double seconds, double fps, int top, bool self)
    {
        std::vector<std::pair<uint64_t, uint32_t>> rows;
        // A function on the stack of every sample (the thread's entry and its wrappers) says nothing.
        for (const auto& [id, counts] : thread.functions)
        {
            if (self || counts.inclusive < thread.busy)
                rows.emplace_back(self ? counts.self : counts.inclusive, id);
        }
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::printf("  %-9s %8s %9s %6s\n", self ? "self" : "inclusive", "ms/s", fps > 0 ? "ms/frame" : "", "%");
        for (int i = 0; i < top && i < int(rows.size()) && rows[i].first > 0; i++)
        {
            double ms = cpuMs * double(rows[i].first) / double(thread.busy);
            double share = 100.0 * double(rows[i].first) / double(thread.busy);
            if (fps > 0)
                std::printf("  %-9s %8.2f %9.3f %6.1f  %s\n", "", ms / seconds, ms / (seconds * fps), share,
                    g_names[rows[i].second].c_str());
            else
                std::printf("  %-9s %8.2f %9s %6.1f  %s\n", "", ms / seconds, "", share, g_names[rows[i].second].c_str());
        }
    }

    BOOL WINAPI OnConsoleControl(DWORD event)
    {
        if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT)
            return FALSE;
        g_stop.store(true);
        return TRUE;
    }

    // Every option takes a value.
    bool ParseOptions(int argc, char** argv, Options& options)
    {
        if (argc % 2 == 0)
            return false;
        for (int i = 1; i + 1 < argc; i += 2)
        {
            std::string key = argv[i];
            const char* value = argv[i + 1];
            if (key == "--pid")
                options.pid = DWORD(std::strtoul(value, nullptr, 10));
            else if (key == "--seconds")
                options.seconds = std::atof(value);
            else if (key == "--rate")
                options.rate = std::max(1, std::atoi(value));
            else if (key == "--top")
                options.top = std::max(1, std::atoi(value));
            else if (key == "--fps")
                options.fps = std::atof(value);
            else if (key == "--names")
                options.names = value;
            else
                return false;
        }
        return options.pid != 0 && options.seconds > 0;
    }
}

int main(int argc, char** argv)
{
    Options options;
    if (!ParseOptions(argc, argv, options))
    {
        std::fprintf(stderr, "usage: roan-sampler --pid <pid> [--seconds 20] [--rate 200] [--top 25] [--fps <rate>] "
                             "[--names <names.tsv>]\n");
        return 2;
    }

    g_process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, options.pid);
    if (!g_process)
    {
        std::fprintf(stderr, "error: cannot open process %lu (error %lu)\n", options.pid, GetLastError());
        return 1;
    }
    if (!options.names.empty())
        LoadGuestNames(options.names);

    // The target's own directory as the only symbol path: its PDB is there, and system modules are
    // named by their exports.
    wchar_t image[MAX_PATH];
    DWORD imageSize = MAX_PATH;
    if (!QueryFullProcessImageNameW(g_process, 0, image, &imageSize))
    {
        std::fprintf(stderr, "error: cannot read the image path of process %lu\n", options.pid);
        return 1;
    }
    std::wstring directory(image);
    directory.resize(directory.find_last_of(L'\\'));
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    if (!SymInitializeW(g_process, directory.c_str(), TRUE))
    {
        std::fprintf(stderr, "error: SymInitialize failed (error %lu)\n", GetLastError());
        return 1;
    }
    // The main module is the first of the target's module list.
    HANDLE modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, options.pid);
    if (modules != INVALID_HANDLE_VALUE)
    {
        MODULEENTRY32W module{};
        module.dwSize = sizeof(module);
        if (Module32FirstW(modules, &module))
            g_mainModule = reinterpret_cast<uint64_t>(module.modBaseAddr);
        CloseHandle(modules);
    }
    g_stack.resize(kMaxStackCopy);

    std::fprintf(stderr, "sampling process %lu for %.0f s at %d rounds a second...\n", options.pid, options.seconds,
        options.rate);
    SetConsoleCtrlHandler(OnConsoleControl, TRUE);
    timeBeginPeriod(1);
    std::map<DWORD, Thread> threads;
    std::vector<Thread> finished;
    LARGE_INTEGER frequency, start, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    uint64_t lastRefresh = 0;
    double elapsed = 0.0;
    uint64_t rounds = 0;
    std::unordered_set<DWORD> runnable;
    while (elapsed < options.seconds && !g_stop.load())
    {
        if (WaitForSingleObject(g_process, 0) == WAIT_OBJECT_0)
        {
            std::fprintf(stderr, "the process exited\n");
            break;
        }
        uint64_t ms = uint64_t(elapsed * 1000.0);
        if (threads.empty() || ms - lastRefresh >= kRefreshMs)
        {
            RefreshThreads(threads, finished);
            SymRefreshModuleList(g_process);
            lastRefresh = ms;
        }
        bool known = RunnableThreads(runnable);
        for (auto& [id, thread] : threads)
        {
            if (!known || runnable.count(id))
                Sample(thread);
            else
                thread.waiting++;
        }
        rounds++;
        QueryPerformanceCounter(&now);
        elapsed = double(now.QuadPart - start.QuadPart) / double(frequency.QuadPart);
        // Rounds run on a fixed schedule, and a round that ran late is followed at once.
        double ahead = double(rounds) / double(options.rate) - elapsed;
        if (ahead > 0.0)
            Sleep(DWORD(ahead * 1000.0));
    }
    timeEndPeriod(1);

    struct Row
    {
        Thread* thread;
        double cpuMs;
    };
    if (rounds == 0 || elapsed <= 0.0)
    {
        std::fprintf(stderr, "error: nothing was sampled\n");
        return 1;
    }
    for (auto& [id, thread] : threads)
        thread.endCpu = ThreadCpu(thread.handle);
    std::vector<Row> rows;
    auto addRow = [&](Thread& thread) {
        double cpuMs = thread.endCpu >= thread.startCpu ? double(thread.endCpu - thread.startCpu) / 1e4 : 0.0;
        rows.push_back({ &thread, cpuMs });
    };
    for (auto& [id, thread] : threads)
        addRow(thread);
    for (Thread& thread : finished)
        addRow(thread);
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.cpuMs > b.cpuMs; });

    std::printf("%.1f s sampled, %.0f rounds a second; ms/s is CPU time a second of the run%s\n", elapsed,
        double(rounds) / elapsed, options.fps > 0 ? ", ms/frame divides it by --fps" : "");
    for (const Row& row : rows)
    {
        const Thread& thread = *row.thread;
        if (thread.busy < kMinBusySamples)
            continue;
        // An unnamed thread is named by the innermost guest function on the stack of all its samples.
        std::string label = thread.label;
        if (label.empty() && !thread.guestPath.empty())
            label = "guest " + g_names[thread.guestPath.back()];
        std::printf("\n== %s (tid %lu): CPU %.2f ms/s", label.empty() ? "unnamed" : label.c_str(), thread.id,
            row.cpuMs / elapsed);
        if (options.fps > 0)
            std::printf(", %.3f ms/frame", row.cpuMs / (elapsed * options.fps));
        std::printf("; %llu samples, %llu more in waits\n", (unsigned long long)thread.busy,
            (unsigned long long)thread.waiting);
        PrintList(thread, row.cpuMs, elapsed, options.fps, options.top, true);
        PrintList(thread, row.cpuMs, elapsed, options.fps, options.top, false);
    }
    for (auto& [id, thread] : threads)
        CloseHandle(thread.handle);
    for (Thread& thread : finished)
        CloseHandle(thread.handle);
    SymCleanup(g_process);
    CloseHandle(g_process);
    return 0;
}
