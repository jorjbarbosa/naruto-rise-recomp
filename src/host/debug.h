#pragma once

// Host debugging: symbols and stack walks of the host code, the crash handler and hardware data
// breakpoints. Recompiled functions are named after their guest address (__imp__sub_XXXXXXXX), so a
// host stack is also the guest call stack.

#include <array>
#include <cstddef>
#include <cstdint>

namespace host
{
    // The registers of a thread stopped by a data breakpoint, as the system hands them to the handler.
    // Valid only while the handler runs.
    struct MachineContext
    {
        uint64_t instruction = 0; // the instruction the thread resumes at
        const void* system = nullptr;
    };

    // What the system reports of a crash. Valid only while the crash handler runs.
    struct Crash
    {
        char description[32] = {};     // the system's name for it ("exception C0000005")
        const void* address = nullptr; // the instruction that crashed
        // An access to memory that is not there or not allowed: the address and whether it was a write.
        bool memoryFault = false;
        const void* data = nullptr;
        bool write = false;
        void* system = nullptr; // the crashed thread's registers, for WalkCrashStack
    };

    // Called on the thread that crashed. It must end the process (std::_Exit): returning gives the crash
    // back to the system, which ends the process without the report.
    using CrashHandler = void (*)(const Crash& crash);

    // Loads the symbols of the process for the walks and FormatSymbol and makes handler the handler of
    // every crash (an exception or fault nothing else handles) on any thread. Call once, early, from the
    // main thread.
    void InstallCrashHandler(CrashHandler handler);

    // The symbol engine is not thread-safe: callers of WalkStack, WalkCrashStack and FormatSymbol
    // serialize their calls, except the crash handler, which ends the process.

    // Writes up to depth return addresses of the stack of context into frames, the first being the
    // context's instruction; returns how many.
    int WalkStack(const MachineContext& context, uint64_t* frames, int depth);

    // As WalkStack, for the crashed thread, walked on its own registers (no copy: a crash may have
    // little stack left); call it once per crash, after the report's first lines are out, since a
    // damaged stack can fault the walk.
    int WalkCrashStack(const Crash& crash, uint64_t* frames, int depth);

    // Writes the symbol of a host address as "name+0xOFFSET", or the address in hex when it has none, into
    // text (cut at size). The first lookup in a module may load its symbols.
    void FormatSymbol(uint64_t address, char* text, size_t size);

    // The debug registers of a thread: four data breakpoints, each on four aligned bytes.
    constexpr uint32_t kDataBreakpoints = 4;

    // Called on the thread that read or wrote a watched word, right after the instruction; hits has bit i
    // set for breakpoint i. The thread then goes on.
    using DataBreakpointHandler = void (*)(uint32_t hits, const MachineContext& context);

    // Makes handler the handler of data breakpoint hits. Call once, before SetDataBreakpoints.
    void InstallDataBreakpointHandler(DataBreakpointHandler handler);

    // Sets the data breakpoints of every thread the process has now, the caller's included (a thread
    // started later has none): breakpoint i on the
    // four bytes at addresses[i] (read or written), none for a null address, so all null clears them. A
    // helper thread does it (a thread cannot change its own debug registers) while the caller waits; each
    // thread is stopped only while its registers are set. Returns false when the threads cannot be
    // listed; otherwise threads is how many there were and set how many took the breakpoints. Any thread.
    bool SetDataBreakpoints(const std::array<void*, kDataBreakpoints>& addresses, uint32_t& set, uint32_t& threads);
}
