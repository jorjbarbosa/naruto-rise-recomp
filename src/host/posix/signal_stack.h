#pragma once

// The alternate signal stack of a thread, on which the crash handler (debug.cpp) runs: a thread that
// overflowed its stack has none left for the handler. POSIX only.

#include <csignal>
#include <cstdlib>

namespace host::posix
{
    // What the crash report needs: its log lines, the unwinder and the symbol lookup.
    constexpr size_t kSignalStackSize = 64 << 10;

    // Gives the calling thread an alternate signal stack for its life; the destructor takes it away
    // (a thread that ends without unwinding keeps it allocated). Owner thread only; a failure leaves
    // the thread without one, so an overflow there ends the process without the report.
    class SignalStack
    {
    public:
        SignalStack()
        {
            memory_ = std::malloc(kSignalStackSize);
            if (memory_ == nullptr)
                return;
            stack_t stack = {};
            stack.ss_sp = memory_;
            stack.ss_size = kSignalStackSize;
            if (sigaltstack(&stack, nullptr) != 0)
            {
                std::free(memory_);
                memory_ = nullptr;
            }
        }

        ~SignalStack()
        {
            if (memory_ == nullptr)
                return;
            stack_t stack = {};
            stack.ss_flags = SS_DISABLE;
            sigaltstack(&stack, nullptr);
            std::free(memory_);
        }

        SignalStack(const SignalStack&) = delete;
        SignalStack& operator=(const SignalStack&) = delete;

    private:
        void* memory_ = nullptr;
    };
}
