// The address space of guest memory: reservation, commit on touch, write record (address_space.h).

#include "host/address_space.h"

#include "log.h"

#include <algorithm>
#include <windows.h>

namespace host
{
    namespace
    {
        // The unit committed by a touch or by CommitAddressSpace. Any multiple of the page size works;
        // 64 KB, the reservation's alignment, means one fault for every 16 pages touched in order.
        constexpr uint64_t kCommitSize = 0x10000;

        uint8_t* g_base = nullptr;
        uint64_t g_size = 0;

        // Commits the reserved pages around a faulting address, kCommitSize at a time.
        LONG CALLBACK CommitOnTouch(EXCEPTION_POINTERS* info)
        {
            if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
                return EXCEPTION_CONTINUE_SEARCH;

            auto* address = reinterpret_cast<uint8_t*>(info->ExceptionRecord->ExceptionInformation[1]);
            if (address < g_base || address >= g_base + g_size)
                return EXCEPTION_CONTINUE_SEARCH;

            auto* page = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(address) & ~uintptr_t(kCommitSize - 1));
            if (VirtualAlloc(page, kCommitSize, MEM_COMMIT, PAGE_READWRITE) == nullptr)
                return EXCEPTION_CONTINUE_SEARCH;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    uint8_t* ReserveAddressSpace(uint64_t size)
    {
        // Offsets and the pages TakeWrittenPages reports are 32-bit.
        if (size > 0x100000000ull)
            return nullptr;
        // MEM_WRITE_WATCH makes the system record which pages are written (TakeWrittenPages).
        g_base = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE));
        if (g_base == nullptr)
            return nullptr;
        g_size = size;
        if (AddVectoredExceptionHandler(1, CommitOnTouch) == nullptr)
        {
            VirtualFree(g_base, 0, MEM_RELEASE);
            g_base = nullptr;
            g_size = 0;
        }
        return g_base;
    }

    void CommitAddressSpace(uint32_t offset, uint32_t size)
    {
        uint64_t begin = offset & ~(kCommitSize - 1);
        uint64_t end = std::min<uint64_t>((uint64_t(offset) + size + kCommitSize - 1) & ~(kCommitSize - 1), g_size);
        if (size == 0 || begin >= end)
            return;
        VirtualAlloc(g_base + begin, size_t(end - begin), MEM_COMMIT, PAGE_READWRITE);
    }

    bool TakeWrittenPages(uint32_t offset, uint32_t size, std::vector<uint32_t>& pages)
    {
        uint64_t begin = offset & ~uint64_t(kWritePageSize - 1);
        uint64_t end = std::min<uint64_t>((uint64_t(offset) + size + kWritePageSize - 1) & ~uint64_t(kWritePageSize - 1), g_size);
        if (size == 0 || begin >= end)
            return true;
        // One entry per page of the range is always enough. The buffer only grows: a resize back up
        // would zero it again on every large query.
        thread_local std::vector<void*> written;
        ULONG_PTR count = ULONG_PTR((end - begin) / kWritePageSize);
        if (written.size() < count)
            written.resize(count);
        ULONG granularity = 0;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, g_base + begin, SIZE_T(end - begin), written.data(), &count,
                &granularity) != 0)
        {
            LOG_ONCE("error: GetWriteWatch failed (error %lu); the pages written are unknown", GetLastError());
            return false;
        }
        for (ULONG_PTR i = 0; i < count; i++)
            pages.push_back(uint32_t(static_cast<uint8_t*>(written[i]) - g_base));
        return true;
    }
}
