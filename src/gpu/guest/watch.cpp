// Write checks of the physical heap over the OS's write watch (mem::TakeWrittenPages).

#include "gpu/guest/watch.h"

#include "gpu/guest/resources.h"
#include "memory.h"

#include <algorithm>
#include <mutex>

namespace gpu::guest
{
    namespace
    {
        // Pages [first, end) of the physical heap.
        struct PageSpan
        {
            uint32_t first = kPhysicalPages;
            uint32_t end = 0;
        };

        // The pages of [address, address + bytes); none for no bytes.
        constexpr PageSpan Pages(uint32_t address, uint32_t bytes)
        {
            uint32_t first = address >> kPageShift;
            return { first, bytes != 0 ? ((address + bytes - 1) >> kPageShift) + 1 : first };
        }

        std::mutex g_mutex; // the state below
        uint64_t g_check = 0;
        std::vector<uint64_t> g_pageChecks(kPhysicalPages);
        bool g_sweepDue = true;
        PageSpan g_asked; // the pages WrittenBySweep was asked about since the last sweep
        PageSpan g_swept; // the pages the last sweep covered

        // Gives the pages of [address, address + bytes) written since the last check that covered them
        // the number of a check. Returns whether there is one. Under g_mutex.
        bool TakeWrites(uint32_t address, uint32_t bytes, uint64_t check)
        {
            thread_local std::vector<uint32_t> written;
            written.clear();
            if (!mem::TakeWrittenPages(GuestPhysical(address), bytes, written))
            {
                // Without the OS's answer, every page counts as written.
                PageSpan pages = Pages(address, bytes);
                std::fill(g_pageChecks.begin() + pages.first, g_pageChecks.begin() + pages.end, check);
                return true;
            }
            for (uint32_t page : written)
                g_pageChecks[(page - GuestPhysical(0)) >> kPageShift] = check;
            return !written.empty();
        }

        // The number of the latest check that found one of the pages written (0: none). Under g_mutex.
        uint64_t LatestWrite(PageSpan pages)
        {
            uint64_t latest = 0;
            for (uint32_t page = pages.first; page < pages.end; page++)
                latest = std::max(latest, g_pageChecks[page]);
            return latest;
        }

        // WrittenBySweep for one range. Under g_mutex.
        bool SweptWritten(uint32_t address, uint32_t bytes, uint64_t since)
        {
            if (g_sweepDue)
            {
                // The frame's sweep: one check of the span of the pages asked about in the last frame.
                g_sweepDue = false;
                g_swept = g_asked;
                g_asked = {};
                if (g_swept.first < g_swept.end)
                    TakeWrites(g_swept.first << kPageShift, (g_swept.end - g_swept.first) << kPageShift, ++g_check);
            }
            PageSpan pages = Pages(address, bytes);
            g_asked.first = std::min(g_asked.first, pages.first);
            g_asked.end = std::max(g_asked.end, pages.end);
            if (pages.first < g_swept.first || pages.end > g_swept.end)
                return true;
            return LatestWrite(pages) > since;
        }
    }

    bool CheckWrites(const std::vector<std::pair<uint32_t, uint32_t>>& ranges, uint64_t since, uint64_t& check)
    {
        std::lock_guard lock(g_mutex);
        check = ++g_check;
        bool stale = false;
        for (const auto& [address, bytes] : ranges)
        {
            stale |= TakeWrites(address, bytes, check);
            // Pages another check found written after `since`.
            stale = stale || LatestWrite(Pages(address, bytes)) > since;
        }
        return stale;
    }

    bool WrittenBySweep(uint32_t address, uint32_t bytes, uint64_t since)
    {
        std::lock_guard lock(g_mutex);
        return SweptWritten(address, bytes, since);
    }

    bool WrittenBySweep(const std::vector<std::pair<uint32_t, uint32_t>>& ranges, uint64_t since)
    {
        std::lock_guard lock(g_mutex);
        // Every range is asked about, so that the next sweep covers them all.
        bool written = false;
        for (const auto& [address, bytes] : ranges)
            written |= SweptWritten(address, bytes, since);
        return written;
    }

    void EndWatchFrame()
    {
        std::lock_guard lock(g_mutex);
        g_sweepDue = true;
    }

    uint64_t LatestCheck()
    {
        std::lock_guard lock(g_mutex);
        return g_check;
    }

    uint64_t LastWriteFound(uint32_t address, uint32_t bytes)
    {
        std::lock_guard lock(g_mutex);
        return LatestWrite(Pages(address, bytes));
    }
}
