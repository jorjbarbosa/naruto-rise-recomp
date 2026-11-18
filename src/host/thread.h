#pragma once

// Host threads: creation with a given stack, names and priority.

#include <cstddef>
#include <cstdint>
#include <functional>

namespace host
{
    // Host stack of the threads that run recompiled code, which recurses deeply.
    constexpr size_t kRecompiledStackSize = 16 << 20;

    // Starts a detached thread that runs body on a stack of stackSize bytes, reserved and committed
    // as it grows. False when the system cannot create the thread; body is then dropped without
    // running. Any thread.
    bool StartDetachedThread(size_t stackSize, std::function<void()> body);

    // Ends the calling thread at once with exitCode, without returning to its callers: their frames
    // are not unwound, so nothing they own may need a destructor. Does not return.
    [[noreturn]] void ExitCurrentThread(uint32_t exitCode);

    // Names the calling thread for debuggers and profilers. The name is ASCII; systems that limit its
    // length cut it. Any thread; a failure is ignored.
    void SetCurrentThreadName(const char* name);

    // Moves the calling thread one step below the normal scheduling priority, for background work
    // that should not take the CPU from the game's threads. Any thread; a failure is ignored.
    void LowerCurrentThreadPriority();
}
