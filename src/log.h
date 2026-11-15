#pragma once

#include <cstdarg>

// Thread-safe logging to stderr.
void Log(const char* format, ...);
void LogV(const char* format, va_list args);
[[noreturn]] void Fatal(const char* format, ...);

// Enabled with the ROAN_TRACE environment variable: logs every kernel call that uses TRACE.
extern bool g_trace;
#define TRACE(...)              \
    do                          \
    {                           \
        if (g_trace)            \
            Log(__VA_ARGS__);   \
    } while (0)
