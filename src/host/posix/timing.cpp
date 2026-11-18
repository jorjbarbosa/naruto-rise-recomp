// The host's clocks and waits on POSIX (timing.h).

#include "host/timing.h"

#include <cerrno>
#include <ctime>
#include <immintrin.h>

namespace host
{
    namespace
    {
        constexpr int64_t kNanosecondsPerSecond = 1'000'000'000;
        // From 1601-01-01 to 1970-01-01, in 100 ns intervals.
        constexpr uint64_t kUnixEpochIn1601 = 116'444'736'000'000'000ull;

        timespec ToTimespec(int64_t nanoseconds)
        {
            return { time_t(nanoseconds / kNanosecondsPerSecond), long(nanoseconds % kNanosecondsPerSecond) };
        }

        // Sleeps until the monotonic clock reaches deadline (nanoseconds), through interruptions.
        void SleepUntilNanoseconds(int64_t deadline)
        {
            timespec until = ToTimespec(deadline);
            while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, nullptr) == EINTR)
            {
            }
        }
    }

    // The counter is the monotonic clock in nanoseconds.
    int64_t Counter()
    {
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        return int64_t(now.tv_sec) * kNanosecondsPerSecond + now.tv_nsec;
    }

    int64_t CounterFrequency()
    {
        return kNanosecondsPerSecond;
    }

    void SleepFor(int64_t ticks)
    {
        SleepUntilNanoseconds(Counter() + (ticks > 0 ? ticks : 1));
    }

    void SleepUntil(int64_t deadline)
    {
        // The same margin as on Windows: the sleep may wake late by the scheduler's latency.
        int64_t margin = CounterFrequency() / 1000;
        if (deadline - Counter() > margin)
            SleepUntilNanoseconds(deadline - margin);
        while (Counter() < deadline)
            _mm_pause();
    }

    uint64_t SystemTime()
    {
        timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        return uint64_t(now.tv_sec) * 10'000'000 + uint64_t(now.tv_nsec) / 100 + kUnixEpochIn1601;
    }
}
