// Kernel data exports and the kernel clock.

#include "kernel/kernel.h"

#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <windows.h>

namespace kernel
{
    ModuleInfo g_module;

    uint64_t SystemTime()
    {
        FILETIME time;
        GetSystemTimePreciseAsFileTime(&time);
        return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    }

    uint64_t TimeBaseFrequency()
    {
        static const uint64_t frequency = [] {
            LARGE_INTEGER qpcFrequency, qpcStart, qpcEnd;
            QueryPerformanceFrequency(&qpcFrequency);
            QueryPerformanceCounter(&qpcStart);
            uint64_t tscStart = __rdtsc();
            Sleep(50);
            QueryPerformanceCounter(&qpcEnd);
            uint64_t tscEnd = __rdtsc();
            double seconds = double(qpcEnd.QuadPart - qpcStart.QuadPart) / double(qpcFrequency.QuadPart);
            return uint64_t(double(tscEnd - tscStart) / seconds);
        }();
        return frequency;
    }

    static uint32_t g_timeStampBundle = 0;

    static uint32_t AllocZeroed(uint32_t size)
    {
        return mem::Alloc(mem::Heap::System, size);
    }

    static uint32_t CreateVariable(const char* module, const char* name)
    {
        if (strcmp(module, "xboxkrnl.exe") != 0)
            return 0;

        if (strcmp(name, "XboxHardwareInfo") == 0)
        {
            uint32_t info = AllocZeroed(16);
            mem::Store32(info + 0, 0);  // flags
            mem::Store8(info + 4, 6);   // number of hardware threads
            return info;
        }
        if (strcmp(name, "XboxKrnlVersion") == 0)
        {
            uint32_t version = AllocZeroed(8);
            mem::Store16(version + 0, 2);
            mem::Store16(version + 2, 0);
            mem::Store16(version + 4, 17559);
            mem::Store16(version + 6, 0);
            return version;
        }
        if (strcmp(name, "KeTimeStampBundle") == 0)
        {
            g_timeStampBundle = AllocZeroed(24);
            return g_timeStampBundle;
        }
        if (strcmp(name, "ExLoadedCommandLine") == 0)
        {
            uint32_t text = AllocZeroed(256);
            strcpy(reinterpret_cast<char*>(mem::Ptr(text)), "\"default.xex\"");
            return text;
        }
        if (strcmp(name, "XexExecutableModuleHandle") == 0)
        {
            // Points to the module handle, whose +0x58 holds the XEX header (RtlImageXexHeaderField).
            uint32_t pointer = AllocZeroed(4);
            mem::Store32(pointer, g_module.moduleHandle);
            return pointer;
        }
        if (strcmp(name, "VdGpuClockInMHz") == 0)
        {
            uint32_t value = AllocZeroed(4);
            mem::Store32(value, 500);
            return value;
        }
        if (strcmp(name, "VdHSIOCalibrationLock") == 0)
        {
            uint32_t cs = AllocZeroed(28);
            mem::Store8(cs, 1);
            mem::Store32(cs + 0x10, 0xFFFFFFFF);
            return cs;
        }
        if (strcmp(name, "VdGlobalDevice") == 0 || strcmp(name, "ExThreadObjectType") == 0 ||
            strcmp(name, "KeDebugMonitorData") == 0 || strcmp(name, "KeCertMonitorData") == 0)
        {
            return AllocZeroed(256);
        }
        return 0;
    }

    // One object per export: default.xex and AI2C.dll (on every load) must see the same one; the
    // kernel clock keeps only the KeTimeStampBundle created first current.
    uint32_t ResolveVariable(const char* module, const char* name)
    {
        static std::mutex mutex;
        static std::unordered_map<std::string, uint32_t> variables;
        std::lock_guard lock(mutex);
        std::string key = std::string(module) + "!" + name;
        auto found = variables.find(key);
        if (found != variables.end())
            return found->second;
        uint32_t address = CreateVariable(module, name);
        if (address != 0)
            variables.emplace(std::move(key), address);
        return address;
    }

    // KeTimeStampBundle: interrupt time, system time (100 ns), tick count (ms).
    void StartKernelClock()
    {
        if (g_timeStampBundle == 0)
            return;
        std::thread([] {
            auto start = std::chrono::steady_clock::now();
            while (true)
            {
                auto elapsed = std::chrono::steady_clock::now() - start;
                uint64_t interrupt = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / 100;
                mem::Store64(g_timeStampBundle + 0, interrupt);
                mem::Store64(g_timeStampBundle + 8, SystemTime());
                mem::Store32(g_timeStampBundle + 16, uint32_t(interrupt / 10000));
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }).detach();
    }
}

using namespace kernel;

GUEST_FUNC(KeQueryPerformanceFrequency)
{
    ctx.r3.u64 = TimeBaseFrequency();
}

GUEST_FUNC(KeQuerySystemTime)
{
    if (ctx.r3.u32 != 0)
        mem::Store64(ctx.r3.u32, SystemTime());
}
