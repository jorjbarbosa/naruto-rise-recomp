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
        std::mutex g_mutex; // the state below
        uint64_t g_check = 0;
        std::vector<uint64_t> g_pageChecks(kPhysicalPages);
    }

    bool CheckWrites(const std::vector<std::pair<uint32_t, uint32_t>>& ranges, uint64_t since, uint64_t& check)
    {
        thread_local std::vector<uint32_t> written;
        std::lock_guard lock(g_mutex);
        check = ++g_check;
        bool stale = false;
        for (const auto& [address, bytes] : ranges)
        {
            uint32_t first = address >> kPageShift;
            uint32_t last = (address + bytes - 1) >> kPageShift;
            written.clear();
            if (!mem::TakeWrittenPages(GuestPhysical(address), bytes, written))
            {
                // Without the OS's answer, every page counts as written.
                std::fill(g_pageChecks.begin() + first, g_pageChecks.begin() + last + 1, check);
                stale = true;
                continue;
            }
            for (uint32_t page : written)
                g_pageChecks[(page - GuestPhysical(0)) >> kPageShift] = check;
            stale |= !written.empty();
            // Pages another copy's check found written after `since`.
            for (uint32_t page = first; !stale && page <= last; page++)
                stale = g_pageChecks[page] > since;
        }
        return stale;
    }

    uint64_t LatestCheck()
    {
        std::lock_guard lock(g_mutex);
        return g_check;
    }

    uint64_t LastWriteFound(uint32_t address, uint32_t bytes)
    {
        std::lock_guard lock(g_mutex);
        uint64_t latest = 0;
        for (uint32_t page = address >> kPageShift; page <= (address + bytes - 1) >> kPageShift; page++)
            latest = std::max(latest, g_pageChecks[page]);
        return latest;
    }
}
