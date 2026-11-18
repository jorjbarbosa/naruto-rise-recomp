#pragma once

// The host's clocks and waits.

#include <cstdint>

namespace host
{
    // The performance counter, in its own ticks. Any thread.
    int64_t Counter();

    // Counter() ticks per second. Any thread.
    int64_t CounterFrequency();

    // Waits about ticks Counter() ticks on a high-resolution timer, without a spin: it may return late
    // by the timer's resolution. A count below one tick waits the shortest time the timer allows. On a
    // thread whose timer could not be made it sleeps 1 ms instead, which the system may stretch to its
    // 15.6 ms granularity. Any thread (each has its own timer).
    void SleepFor(int64_t ticks);

    // Waits until Counter() reaches deadline: a high-resolution timer to about 1 ms before it, then a
    // spin (a plain sleep has the system's 15.6 ms granularity). Returns at once when it has passed.
    // Any thread.
    void SleepUntil(int64_t deadline);

    // The wall clock in 100 ns intervals since 1601-01-01 UTC, the system time of the Xbox kernel.
    // Any thread.
    uint64_t SystemTime();
}
