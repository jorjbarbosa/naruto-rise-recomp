// Runtime library: strings, formatting, time, exceptions, XEX headers, modules and settings.

#include "kernel/kernel.h"

#include "ascii.h"
#include "host/input.h"
#include "loader.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwctype>

namespace kernel
{
    std::string LoadAnsiString(uint32_t address)
    {
        if (address == 0)
            return {};
        uint16_t length = mem::Load16(address);
        uint32_t buffer = mem::Load32(address + 4);
        if (buffer == 0)
            return {};
        return std::string(reinterpret_cast<const char*>(mem::Ptr(buffer)), length);
    }
}

using namespace kernel;

// ---- Formatting (sprintf family exported by the kernel) ----

namespace
{
    // Variadic arguments occupy one 8-byte slot each: r3-r10 first, then the caller's parameter area.
    class ArgSource
    {
    public:
        virtual ~ArgSource() = default;
        virtual uint64_t Next() = 0;
    };

    class RegisterArgs : public ArgSource
    {
    public:
        RegisterArgs(const PPCContext& ctx, int firstIndex) : m_ctx(ctx), m_index(firstIndex) {}

        uint64_t Next() override
        {
            int i = m_index++;
            switch (i)
            {
            case 0: return m_ctx.r3.u64;
            case 1: return m_ctx.r4.u64;
            case 2: return m_ctx.r5.u64;
            case 3: return m_ctx.r6.u64;
            case 4: return m_ctx.r7.u64;
            case 5: return m_ctx.r8.u64;
            case 6: return m_ctx.r9.u64;
            case 7: return m_ctx.r10.u64;
            default: return mem::Load64(m_ctx.r1.u32 + 0x50 + (i - 8) * 8);
            }
        }

    private:
        const PPCContext& m_ctx;
        int m_index;
    };

    class VaListArgs : public ArgSource
    {
    public:
        explicit VaListArgs(uint32_t address) : m_address(address) {}

        uint64_t Next() override
        {
            uint64_t value = mem::Load64(m_address);
            m_address += 8;
            return value;
        }

    private:
        uint32_t m_address;
    };

    std::string WideToNarrow(uint32_t address)
    {
        std::string text;
        if (address == 0)
            return "(null)";
        for (uint16_t c; (c = mem::Load16(address)) != 0; address += 2)
            text += c < 0x80 ? char(c) : '?';
        return text;
    }

    std::string Format(uint32_t formatAddress, ArgSource& args)
    {
        const char* format = mem::String(formatAddress);
        std::string out;
        char buffer[512];

        for (const char* p = format; *p != 0; p++)
        {
            if (*p != '%')
            {
                out += *p;
                continue;
            }

            std::string spec = "%";
            p++;
            while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0')
                spec += *p++;
            if (*p == '*')
            {
                spec += std::to_string(int32_t(args.Next()));
                p++;
            }
            while (*p >= '0' && *p <= '9')
                spec += *p++;
            if (*p == '.')
            {
                spec += *p++;
                if (*p == '*')
                {
                    spec += std::to_string(int32_t(args.Next()));
                    p++;
                }
                while (*p >= '0' && *p <= '9')
                    spec += *p++;
            }

            int size = 0; // -2 hh, -1 h, 0 default, 1 l, 2 ll
            bool wide = false;
            if (p[0] == 'h' && p[1] == 'h') { size = -2; p += 2; }
            else if (*p == 'h') { size = -1; p++; }
            else if (p[0] == 'l' && p[1] == 'l') { size = 2; p += 2; }
            else if (*p == 'l') { size = 1; wide = true; p++; }
            else if (*p == 'w') { wide = true; p++; }
            else if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { size = 2; p += 3; }
            else if (p[0] == 'I' && p[1] == '3' && p[2] == '2') { p += 3; }
            else if (*p == 'I' || *p == 'z' || *p == 'j' || *p == 't' || *p == 'L') { p++; }

            char conversion = *p;
            if (conversion == 0)
                break;

            switch (conversion)
            {
            case 'd':
            case 'i':
            {
                uint64_t raw = args.Next();
                int64_t value = size == 2 ? int64_t(raw) : size == -1 ? int16_t(raw) : size == -2 ? int8_t(raw) : int32_t(raw);
                snprintf(buffer, sizeof(buffer), (spec + "lld").c_str(), (long long)value);
                out += buffer;
                break;
            }
            case 'u':
            case 'x':
            case 'X':
            case 'o':
            {
                uint64_t raw = args.Next();
                uint64_t value = size == 2 ? raw : size == -1 ? uint16_t(raw) : size == -2 ? uint8_t(raw) : uint32_t(raw);
                snprintf(buffer, sizeof(buffer), (spec + "ll" + conversion).c_str(), (unsigned long long)value);
                out += buffer;
                break;
            }
            case 'c':
            case 'C':
                snprintf(buffer, sizeof(buffer), (spec + "c").c_str(), int(char(args.Next())));
                out += buffer;
                break;
            case 's':
            case 'S':
            {
                uint32_t address = uint32_t(args.Next());
                bool isWide = conversion == 'S' ? !wide || size != 1 : wide;
                std::string text = isWide ? WideToNarrow(address) : std::string(mem::String(address));
                snprintf(buffer, sizeof(buffer), (spec + "s").c_str(), text.c_str());
                out += buffer;
                break;
            }
            case 'p':
                snprintf(buffer, sizeof(buffer), "%08X", uint32_t(args.Next()));
                out += buffer;
                break;
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
            case 'a':
            case 'A':
            {
                uint64_t raw = args.Next();
                double value;
                memcpy(&value, &raw, sizeof(value));
                snprintf(buffer, sizeof(buffer), (spec + conversion).c_str(), value);
                out += buffer;
                break;
            }
            case 'n':
                args.Next();
                break;
            case '%':
                out += '%';
                break;
            default:
                out += spec;
                out += conversion;
                break;
            }
        }
        return out;
    }

    // MSVC _snprintf semantics: no terminator when the text fills the buffer, -1 when truncated.
    int32_t StoreFormatted(uint32_t buffer, uint32_t count, const std::string& text)
    {
        uint32_t length = uint32_t(text.size());
        uint32_t copy = std::min(length, count);
        memcpy(mem::Ptr(buffer), text.data(), copy);
        if (length < count)
            mem::Store8(buffer + length, 0);
        return length <= count ? int32_t(length) : -1;
    }
}

GUEST_FUNC(sprintf)
{
    RegisterArgs args(ctx, 2);
    std::string text = Format(ctx.r4.u32, args);
    memcpy(mem::Ptr(ctx.r3.u32), text.c_str(), text.size() + 1);
    ctx.r3.u64 = text.size();
}

GUEST_FUNC(_snprintf)
{
    RegisterArgs args(ctx, 3);
    std::string text = Format(ctx.r5.u32, args);
    ctx.r3.s64 = StoreFormatted(ctx.r3.u32, ctx.r4.u32, text);
}

GUEST_FUNC(_vsnprintf)
{
    VaListArgs args(ctx.r6.u32);
    std::string text = Format(ctx.r5.u32, args);
    ctx.r3.s64 = StoreFormatted(ctx.r3.u32, ctx.r4.u32, text);
}

GUEST_FUNC(DbgPrint)
{
    RegisterArgs args(ctx, 1);
    std::string text = Format(ctx.r3.u32, args);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    Log("[DbgPrint] %s", text.c_str());
    ctx.r3.u64 = 0;
}

GUEST_FUNC(DbgBreakPoint)
{
    Log("DbgBreakPoint (lr=%08X)", uint32_t(ctx.lr));
}

// ---- Strings ----

// RtlInitAnsiString(ANSI_STRING*, source)
GUEST_FUNC(RtlInitAnsiString)
{
    uint32_t string = ctx.r3.u32;
    uint32_t source = ctx.r4.u32;
    uint16_t length = source != 0 ? uint16_t(strlen(mem::String(source))) : 0;
    mem::Store16(string + 0, length);
    mem::Store16(string + 2, source != 0 ? uint16_t(length + 1) : 0);
    mem::Store32(string + 4, source);
}

// RtlCompareStringN(string1, length1, string2, length2, caseInsensitive)
GUEST_FUNC(RtlCompareStringN)
{
    const char* a = mem::String(ctx.r3.u32);
    const char* b = mem::String(ctx.r5.u32);
    uint32_t lengthA = ctx.r4.u32 == 0xFFFFFFFF ? uint32_t(strlen(a)) : ctx.r4.u32;
    uint32_t lengthB = ctx.r6.u32 == 0xFFFFFFFF ? uint32_t(strlen(b)) : ctx.r6.u32;
    uint32_t length = std::min(lengthA, lengthB);
    int result = ctx.r7.u32 ? ascii::CompareNoCase(a, b, length) : strncmp(a, b, length);
    ctx.r3.s64 = result != 0 ? result : int32_t(lengthA) - int32_t(lengthB);
}

GUEST_FUNC(RtlUpcaseUnicodeChar)
{
    ctx.r3.u64 = uint16_t(towupper(wchar_t(ctx.r3.u32 & 0xFFFF)));
}

// RtlMultiByteToUnicodeN(unicode, maxBytes, bytesWritten*, multiByte, multiByteBytes)
GUEST_FUNC(RtlMultiByteToUnicodeN)
{
    uint32_t count = std::min(ctx.r4.u32 / 2, ctx.r7.u32);
    for (uint32_t i = 0; i < count; i++)
        mem::Store16(ctx.r3.u32 + i * 2, mem::Load8(ctx.r6.u32 + i));
    if (ctx.r5.u32 != 0)
        mem::Store32(ctx.r5.u32, count * 2);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// RtlUnicodeToMultiByteN(multiByte, maxBytes, bytesWritten*, unicode, unicodeBytes)
GUEST_FUNC(RtlUnicodeToMultiByteN)
{
    uint32_t count = std::min(ctx.r4.u32, ctx.r7.u32 / 2);
    for (uint32_t i = 0; i < count; i++)
    {
        uint16_t c = mem::Load16(ctx.r6.u32 + i * 2);
        mem::Store8(ctx.r3.u32 + i, c < 0x100 ? uint8_t(c) : '?');
    }
    if (ctx.r5.u32 != 0)
        mem::Store32(ctx.r5.u32, count);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// RtlFillMemoryUlong(destination, length, pattern)
GUEST_FUNC(RtlFillMemoryUlong)
{
    for (uint32_t i = 0; i + 4 <= ctx.r4.u32; i += 4)
        mem::Store32(ctx.r3.u32 + i, ctx.r5.u32);
}

// RtlCompareMemoryUlong(source, length, pattern) -> bytes that match
GUEST_FUNC(RtlCompareMemoryUlong)
{
    uint32_t matched = 0;
    while (matched + 4 <= ctx.r4.u32 && mem::Load32(ctx.r3.u32 + matched) == ctx.r5.u32)
        matched += 4;
    ctx.r3.u64 = matched;
}

GUEST_FUNC(RtlNtStatusToDosError)
{
    uint32_t status = ctx.r3.u32;
    uint32_t error;
    switch (status)
    {
    case X_STATUS_SUCCESS: error = 0; break;
    case X_STATUS_PENDING: error = X_ERROR_IO_PENDING; break;
    case X_STATUS_NO_SUCH_FILE:
    case X_STATUS_OBJECT_NAME_NOT_FOUND: error = X_ERROR_FILE_NOT_FOUND; break;
    case X_STATUS_OBJECT_PATH_NOT_FOUND: error = X_ERROR_PATH_NOT_FOUND; break;
    case X_STATUS_ACCESS_DENIED: error = X_ERROR_ACCESS_DENIED; break;
    case X_STATUS_INVALID_HANDLE: error = 6; break;
    case X_STATUS_NO_MEMORY: error = 8; break;
    case X_STATUS_END_OF_FILE: error = 38; break;
    case X_STATUS_INVALID_PARAMETER: error = X_ERROR_INVALID_PARAMETER; break;
    case X_STATUS_OBJECT_NAME_COLLISION: error = 183; break;
    case X_STATUS_NO_MORE_FILES: error = X_ERROR_NO_MORE_FILES; break;
    case X_STATUS_BUFFER_TOO_SMALL: error = X_ERROR_INSUFFICIENT_BUFFER; break;
    default:
        error = (status & 0xFFFF0000) == 0x80070000 ? status & 0xFFFF : 317;
        break;
    }
    ctx.r3.u64 = error;
}

// ---- Time ----

namespace
{
    // System times count 100 ns intervals from the first day of 1601 (UTC).
    constexpr uint64_t kTicksPerMillisecond = 10'000;
    constexpr uint64_t kMillisecondsPerDay = 86'400'000;
    constexpr uint64_t kTicksPerDay = kMillisecondsPerDay * kTicksPerMillisecond;
    constexpr std::chrono::sys_days kFirstDay{ std::chrono::year(1601) / std::chrono::January / 1 };
    // The last year a set of fields may give, as in SystemTimeToFileTime.
    constexpr uint32_t kLastYear = 30827;
}

// RtlTimeToTimeFields(time*, TIME_FIELDS*): TIME_FIELDS is eight 16-bit fields, year, month, day,
// hour, minute, second, milliseconds and weekday (0 is Sunday).
GUEST_FUNC(RtlTimeToTimeFields)
{
    uint64_t time = mem::Load64(ctx.r3.u32);
    uint16_t values[8] = {};
    // Times from 2^63 on are negative as the kernel's LARGE_INTEGER: their fields are left zero.
    if (time < (uint64_t(1) << 63))
    {
        std::chrono::sys_days day = kFirstDay + std::chrono::days(time / kTicksPerDay);
        std::chrono::year_month_day date(day);
        uint64_t milliseconds = time % kTicksPerDay / kTicksPerMillisecond;
        values[0] = uint16_t(int(date.year()));
        values[1] = uint16_t(unsigned(date.month()));
        values[2] = uint16_t(unsigned(date.day()));
        values[3] = uint16_t(milliseconds / 3'600'000);
        values[4] = uint16_t(milliseconds / 60'000 % 60);
        values[5] = uint16_t(milliseconds / 1'000 % 60);
        values[6] = uint16_t(milliseconds % 1'000);
        values[7] = uint16_t(std::chrono::weekday(day).c_encoding());
    }
    for (uint32_t i = 0; i < 8; i++)
        mem::Store16(ctx.r4.u32 + 2 * i, values[i]);
}

// RtlTimeFieldsToTime(TIME_FIELDS*, time*) -> success: fails, writing nothing, on a field out of its
// range (the weekday is not read).
GUEST_FUNC(RtlTimeFieldsToTime)
{
    uint32_t fields = ctx.r3.u32;
    uint32_t year = mem::Load16(fields + 0);
    uint32_t month = mem::Load16(fields + 2);
    uint32_t day = mem::Load16(fields + 4);
    uint32_t hour = mem::Load16(fields + 6);
    uint32_t minute = mem::Load16(fields + 8);
    uint32_t second = mem::Load16(fields + 10);
    uint32_t milliseconds = mem::Load16(fields + 12);
    ctx.r3.u64 = 0;
    if (year < 1601 || year > kLastYear || hour > 23 || minute > 59 || second > 59 || milliseconds > 999)
        return;
    std::chrono::year_month_day date{ std::chrono::year(int(year)), std::chrono::month(month),
                                      std::chrono::day(day) };
    if (!date.ok())
        return;
    uint64_t days = uint64_t((std::chrono::sys_days(date) - kFirstDay).count());
    uint64_t dayMilliseconds = ((hour * 60 + minute) * 60 + second) * 1'000 + milliseconds;
    mem::Store64(ctx.r4.u32, days * kTicksPerDay + dayMilliseconds * kTicksPerMillisecond);
    ctx.r3.u64 = 1;
}

// ---- Exceptions ----

// RtlRaiseException(EXCEPTION_RECORD*): only the thread-naming exception is expected.
GUEST_FUNC(RtlRaiseException)
{
    uint32_t record = ctx.r3.u32;
    uint32_t code = mem::Load32(record);
    if (code == 0x406D1388 && mem::Load32(record + 0x14) == 0x1000)
    {
        std::string name = mem::String(mem::Load32(record + 0x18));
        uint32_t id = mem::Load32(record + 0x1C);
        GuestThread* thread = id == 0xFFFFFFFF ? CurrentThread() : ThreadById(id);
        if (thread != nullptr)
            thread->name = name;
        Log("thread %u is \"%s\"", thread ? thread->id : id, name.c_str());
        return;
    }
    Log("warning: RtlRaiseException code %08X at %08X ignored (lr=%08X)", code, mem::Load32(record + 0xC), uint32_t(ctx.lr));
}

GUEST_FUNC(RtlUnwind)
{
    Fatal("fatal: RtlUnwind called (C++ exceptions are not supported), lr=%08X", uint32_t(ctx.lr));
}

GUEST_FUNC(RtlCaptureContext) {}

GUEST_FUNC(KeBugCheck)
{
    Fatal("fatal: KeBugCheck(%08X), lr=%08X", ctx.r3.u32, uint32_t(ctx.lr));
}

GUEST_FUNC(KeBugCheckEx)
{
    Fatal("fatal: KeBugCheckEx(%08X, %08X, %08X, %08X, %08X), lr=%08X",
        ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, uint32_t(ctx.lr));
}

GUEST_FUNC(HalReturnToFirmware)
{
    host::StopInput();
    PrintImportSummary();
    Log("HalReturnToFirmware(%u): the game asked to leave", ctx.r3.u32);
    std::_Exit(0);
}

GUEST_FUNC(KeEnableFpuExceptions) {}
GUEST_FUNC(ExRegisterTitleTerminateNotification) {}

// ---- XEX headers and modules ----

// RtlImageXexHeaderField(xexHeader, key) -> value or guest pointer
GUEST_FUNC(RtlImageXexHeaderField)
{
    uint32_t header = ctx.r3.u32;
    uint32_t key = ctx.r4.u32;
    uint32_t count = mem::Load32(header + 0x14);
    ctx.r3.u64 = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t entry = header + 0x18 + i * 8;
        if (mem::Load32(entry) != key)
            continue;
        uint32_t value = mem::Load32(entry + 4);
        switch (key & 0xFF)
        {
        case 0x00: ctx.r3.u64 = value; break;
        case 0x01: ctx.r3.u64 = entry + 4; break;
        default: ctx.r3.u64 = header + value; break;
        }
        return;
    }
}

GUEST_FUNC(XexCheckExecutablePrivilege) { ctx.r3.u64 = 0; }

// XexGetModuleHandle(name, handle*)
GUEST_FUNC(XexGetModuleHandle)
{
    static uint32_t xamHandle = 0;
    static uint32_t kernelHandle = 0;
    std::string name = ctx.r3.u32 != 0 ? mem::String(ctx.r3.u32) : "";
    uint32_t handle = 0;
    if (name.empty() || ascii::EqualNoCase(name, "default.xex"))
        handle = g_module.moduleHandle;
    else if (ascii::EqualNoCase(name, "xam.xex"))
        handle = xamHandle != 0 ? xamHandle : (xamHandle = mem::Alloc(mem::Heap::System, 0x100));
    else if (ascii::EqualNoCase(name, "xboxkrnl.exe"))
        handle = kernelHandle != 0 ? kernelHandle : (kernelHandle = mem::Alloc(mem::Heap::System, 0x100));

    TRACE("XexGetModuleHandle(\"%s\") -> %08X", name.c_str(), handle);
    mem::Store32(ctx.r4.u32, handle);
    ctx.r3.u64 = handle != 0 ? X_STATUS_SUCCESS : X_STATUS_DLL_NOT_FOUND;
}

// XexGetProcedureAddress(module, ordinal, address*): exports of the recompiled DLLs. The game also
// asks the XAM handle for XNet ordinals, which stay refused (it is offline).
GUEST_FUNC(XexGetProcedureAddress)
{
    uint32_t address = FindModuleExport(ctx.r3.u32, ctx.r4.u32);
    if (address == 0)
        Log("XexGetProcedureAddress(%08X, ordinal %u) refused (lr=%08X)", ctx.r3.u32, ctx.r4.u32, uint32_t(ctx.lr));
    mem::Store32(ctx.r5.u32, address);
    ctx.r3.u64 = address != 0 ? X_STATUS_SUCCESS : X_STATUS_DRIVER_ORDINAL_NOT_FOUND;
}

// XexLoadImage(name, flags, minimumVersion, handle*). The game loads AI2C.dll, its AI scripts compiled
// to C, and cannot run without it (it has no AI interpreter); only recompiled modules load.
GUEST_FUNC(XexLoadImage)
{
    std::string name = mem::String(ctx.r3.u32);
    uint32_t handleOut = ctx.r6.u32;
    uint32_t status;
    uint32_t handle = LoadGuestModule(name, ctx, base, status);
    if (handleOut != 0)
        mem::Store32(handleOut, handle);
    ctx.r3.u64 = status;
}

GUEST_FUNC(XexLoadImageHeaders)
{
    Log("XexLoadImageHeaders(\"%s\") refused", LoadAnsiString(ctx.r3.u32).c_str());
    ctx.r3.u64 = X_STATUS_NO_SUCH_FILE;
}

GUEST_FUNC(XexUnloadImage) { ctx.r3.u64 = UnloadGuestModule(ctx.r3.u32, ctx, base) ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE; }

// ---- Console settings ----

// ExGetXConfigSetting(category, setting, buffer, bufferSize, requiredSize*)
GUEST_FUNC(ExGetXConfigSetting)
{
    uint16_t category = uint16_t(ctx.r3.u32);
    uint16_t setting = uint16_t(ctx.r4.u32);
    uint32_t buffer = ctx.r5.u32;
    uint32_t bufferSize = ctx.r6.u32;
    uint32_t requiredSizePtr = ctx.r7.u32;

    uint8_t value[16] = {};
    uint32_t size = 0;
    auto set32 = [&](uint32_t v) { size = 4; value[0] = uint8_t(v >> 24); value[1] = uint8_t(v >> 16); value[2] = uint8_t(v >> 8); value[3] = uint8_t(v); };

    if (category == 0x0002 && setting == 0x0002) set32(0x00001000);      // AV region: NTSC-U
    else if (category == 0x0003 && setting == 0x0009) set32(1);          // language: English
    else if (category == 0x0003 && setting == 0x000A) set32(0x00040000); // video flags
    else if (category == 0x0003 && setting == 0x000B) set32(0x00010001); // audio flags
    else if (category == 0x0003 && setting == 0x000C) set32(0x00000040); // retail flags
    else if (category == 0x0003 && setting == 0x000E) { size = 1; value[0] = 103; } // country: USA
    else if (category == 0x0003 && setting <= 0x0007) set32(0);          // time zone
    else
    {
        Log("warning: ExGetXConfigSetting(%04X, %04X) unknown", category, setting);
        ctx.r3.u64 = X_STATUS_INVALID_PARAMETER;
        return;
    }

    if (buffer != 0 && bufferSize >= size)
        memcpy(mem::Ptr(buffer), value, size);
    if (requiredSizePtr != 0)
        mem::Store16(requiredSizePtr, uint16_t(size));
    ctx.r3.u64 = (buffer != 0 && bufferSize < size) ? X_STATUS_BUFFER_TOO_SMALL : X_STATUS_SUCCESS;
}
