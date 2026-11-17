// The host's high-resolution clock and wait (timing.h).

#include "host/timing.h"

#include <windows.h>

namespace host
{
    int64_t Counter()
    {
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        return counter.QuadPart;
    }

    int64_t CounterFrequency()
    {
        static const int64_t frequency = [] {
            LARGE_INTEGER value;
            QueryPerformanceFrequency(&value);
            return value.QuadPart;
        }();
        return frequency;
    }

    void SleepUntil(int64_t deadline)
    {
        // One timer per thread: a waitable timer set from two threads would wake only one of them.
        thread_local HANDLE timer =
            CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        int64_t frequency = CounterFrequency();
        int64_t margin = frequency / 1000;
        int64_t now = Counter();
        if (timer != nullptr && deadline - now > margin)
        {
            LARGE_INTEGER due;
            due.QuadPart = -((deadline - now - margin) * 10'000'000 / frequency);
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
                WaitForSingleObject(timer, INFINITE);
        }
        while (Counter() < deadline)
            YieldProcessor();
    }
}
