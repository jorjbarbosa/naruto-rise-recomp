#include "log.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>

bool g_trace = false;

static std::mutex g_logMutex;

void LogV(const char* format, va_list args)
{
    std::lock_guard lock(g_logMutex);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    fflush(stderr);
}

void Log(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    LogV(format, args);
    va_end(args);
}

void Fatal(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    LogV(format, args);
    va_end(args);
    fflush(stderr);
    std::_Exit(1);
}
