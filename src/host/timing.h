#pragma once

// The host's high-resolution clock and wait.

#include <cstdint>

namespace host
{
    // The performance counter, in its own ticks. Any thread.
    int64_t Counter();

    // Counter() ticks per second. Any thread.
    int64_t CounterFrequency();

    // A duration in seconds as Counter() ticks. Any thread.
    int64_t CounterTicks(double seconds);

    // Waits until Counter() reaches deadline: a high-resolution timer to about 1 ms before it, then a
    // spin (a plain sleep has the system's 15.6 ms granularity). Returns at once when it has passed.
    // Any thread (each has its own timer).
    void SleepUntil(int64_t deadline);
}
