// The host's clocks and waits on Windows (timing.h).

#include "host/timing.h"

#include <algorithm>
#include <windows.h>

namespace host
{
    namespace
    {
        // Waits ticks Counter() ticks (at least 100 ns) on the thread's high-resolution timer. False
        // when the timer could not be made or set, without waiting. One timer per thread: a waitable
        // timer set from two threads would wake only one of them.
        bool WaitOnTimer(int64_t ticks)
        {
            thread_local HANDLE timer =
                CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (timer == nullptr)
                return false;
            LARGE_INTEGER due;
            due.QuadPart = -std::max<int64_t>(ticks * 10'000'000 / CounterFrequency(), 1);
            if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
                return false;
            WaitForSingleObject(timer, INFINITE);
            return true;
        }
    }

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

    void SleepFor(int64_t ticks)
    {
        if (!WaitOnTimer(ticks))
            Sleep(1);
    }

    void SleepUntil(int64_t deadline)
    {
        int64_t margin = CounterFrequency() / 1000;
        int64_t now = Counter();
        if (deadline - now > margin)
            WaitOnTimer(deadline - now - margin);
        while (Counter() < deadline)
            YieldProcessor();
    }

    uint64_t SystemTime()
    {
        FILETIME time;
        GetSystemTimePreciseAsFileTime(&time);
        return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    }
}
