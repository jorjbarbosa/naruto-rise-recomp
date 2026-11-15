#pragma once

#include <atomic>
#include <cstdarg>

// Thread-safe logging to stderr.
void Log(const char* format, ...);
void LogV(const char* format, va_list args);
[[noreturn]] void Fatal(const char* format, ...);

// Logs only the first time this line is reached.
#define LOG_ONCE(...)                                           \
    do                                                          \
    {                                                           \
        static std::atomic<bool> logged_{ false };              \
        if (!logged_.exchange(true, std::memory_order_relaxed)) \
            Log(__VA_ARGS__);                                   \
    } while (0)

// Enabled with the ROAN_TRACE environment variable: logs every kernel call that uses TRACE.
extern bool g_trace;
#define TRACE(...)              \
    do                          \
    {                           \
        if (g_trace)            \
            Log(__VA_ARGS__);   \
    } while (0)
