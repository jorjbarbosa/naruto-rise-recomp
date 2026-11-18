#pragma once

#include <atomic>
#include <cstdarg>

// Thread-safe logging to stderr. The formats are printf's, checked by the compiler: a type that
// matches on one system may not on another (uint64_t is unsigned long long on Windows and unsigned
// long on Linux).
void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));
void LogV(const char* format, va_list args);
[[noreturn]] void Fatal(const char* format, ...) __attribute__((format(printf, 1, 2)));

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
