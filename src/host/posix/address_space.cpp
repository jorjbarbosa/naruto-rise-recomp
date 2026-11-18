// The address space of guest memory on Linux: reservation, commit on touch, write record (address_space.h).

#include "host/address_space.h"

#include "log.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace host
{
    namespace
    {
        // The reservation's alignment, as on Windows (its allocation granularity).
        constexpr uint64_t kAlignment = 0x10000;

        // The regions one PAGEMAP_SCAN call returns at most; a longer answer takes more calls.
        constexpr size_t kScanRegions = 256;

        uint8_t* g_base = nullptr;
        uint64_t g_size = 0;
        // /proc/self/pagemap, which answers and resets the write record; -1 without one.
        int g_pagemap = -1;

        // Sets up the write record of the reservation: every page write-protected through a
        // userfaultfd in asynchronous mode, where the kernel itself lifts the protection of a page on
        // its first write (from user code or from a system call writing into it, so no handler and no
        // failing read()) and PAGEMAP_SCAN reports and protects again the pages that lost it. This is
        // how Wine implements GetWriteWatch. Needs Linux 6.7 to run, and its headers to build. Logs and
        // returns false on a failure.
        bool StartWriteRecord()
        {
            // Huge pages would be reported whole: 2 MB written for one byte. A kernel without them
            // refuses the advice with EINVAL, and has nothing to turn off.
            if (madvise(g_base, g_size, MADV_NOHUGEPAGE) != 0 && errno != EINVAL)
                Log("warning: madvise(MADV_NOHUGEPAGE) failed (errno %d); written pages may be reported 2 MB at a time", errno);

            // User mode only: the default for an unprivileged process (vm.unprivileged_userfaultfd = 0),
            // and the asynchronous mode resolves the kernel's own writes before it would matter.
            int uffd = int(syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY));
            if (uffd < 0)
            {
                Log("error: userfaultfd failed (errno %d); the pages written are unknown", errno);
                return false;
            }
            uffdio_api api = {};
            api.api = UFFD_API;
            api.features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED;
            uffdio_register range = {};
            range.range.start = reinterpret_cast<uintptr_t>(g_base);
            range.range.len = g_size;
            range.mode = UFFDIO_REGISTER_MODE_WP;
            uffdio_writeprotect protect = {};
            protect.range = range.range;
            protect.mode = UFFDIO_WRITEPROTECT_MODE_WP;
            const char* failed = nullptr;
            if (ioctl(uffd, UFFDIO_API, &api) != 0)
                failed = "UFFDIO_API (asynchronous write protection)";
            else if (ioctl(uffd, UFFDIO_REGISTER, &range) != 0)
                failed = "UFFDIO_REGISTER";
            else if (ioctl(uffd, UFFDIO_WRITEPROTECT, &protect) != 0)
                failed = "UFFDIO_WRITEPROTECT";
            else if ((g_pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC)) < 0)
                failed = "opening /proc/self/pagemap";
            if (failed != nullptr)
            {
                Log("error: %s failed (errno %d); the pages written are unknown", failed, errno);
                close(uffd);
                return false;
            }
            // The userfaultfd stays open for the life of the process: closing it would unregister the
            // range and end the write protection.
            return true;
        }
    }

    uint8_t* ReserveAddressSpace(uint64_t size)
    {
        // Offsets and the pages TakeWrittenPages reports are 32-bit.
        if (size > 0x100000000ull)
            return nullptr;
        // MAP_NORESERVE: the system gives a page its memory when it is first touched, by anyone, the
        // system's own calls (read into it) included, so there is nothing to commit by hand.
        uint64_t mapped = size + kAlignment;
        void* map = mmap(nullptr, mapped, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (map == MAP_FAILED)
            return nullptr;
        // Keep the aligned part and give the rest back.
        auto start = reinterpret_cast<uintptr_t>(map);
        uintptr_t base = (start + kAlignment - 1) & ~uintptr_t(kAlignment - 1);
        if (base > start)
            munmap(map, base - start);
        uintptr_t end = start + mapped;
        if (end > base + size)
            munmap(reinterpret_cast<void*>(base + size), end - (base + size));
        g_base = reinterpret_cast<uint8_t*>(base);
        g_size = size;
        // Without a write record the space still works; TakeWrittenPages then fails.
        StartWriteRecord();
        return g_base;
    }

    void CommitAddressSpace(uint32_t, uint32_t)
    {
        // Committed on touch (ReserveAddressSpace).
    }

    bool TakeWrittenPages(uint32_t offset, uint32_t size, std::vector<uint32_t>& pages)
    {
        uint64_t begin = offset & ~uint64_t(kWritePageSize - 1);
        uint64_t end = std::min<uint64_t>((uint64_t(offset) + size + kWritePageSize - 1) & ~uint64_t(kWritePageSize - 1), g_size);
        if (size == 0 || begin >= end)
            return true;
        // StartWriteRecord logged why there is no record.
        if (g_pagemap < 0)
            return false;

        // Reports the written pages and write-protects them again in one step, page by page under the
        // kernel's page table lock, so a write racing with the call is either reported now or later.
        page_region regions[kScanRegions];
        pm_scan_arg scan = {};
        scan.size = sizeof(scan);
        scan.flags = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC;
        scan.start = reinterpret_cast<uintptr_t>(g_base + begin);
        scan.end = reinterpret_cast<uintptr_t>(g_base + end);
        scan.vec = reinterpret_cast<uintptr_t>(regions);
        scan.vec_len = kScanRegions;
        scan.category_mask = PAGE_IS_WRITTEN;
        scan.return_mask = PAGE_IS_WRITTEN;
        size_t reported = pages.size();
        auto base = reinterpret_cast<uintptr_t>(g_base);
        while (scan.start < scan.end)
        {
            long count = ioctl(g_pagemap, PAGEMAP_SCAN, &scan);
            if (count < 0)
                LOG_ONCE("error: PAGEMAP_SCAN failed (errno %d); the pages written are unknown", errno);
            // A scan that stops where it began would never end.
            else if (scan.walk_end <= scan.start)
                LOG_ONCE("error: PAGEMAP_SCAN stopped at %llX; the pages written are unknown",
                    (unsigned long long)scan.start);
            if (count < 0 || scan.walk_end <= scan.start)
            {
                // The pages taken so far were reset and are not reported: the caller counts the
                // whole range as written.
                pages.resize(reported);
                return false;
            }
            for (long i = 0; i < count; i++)
                for (uint64_t page = regions[i].start; page < regions[i].end; page += kWritePageSize)
                    pages.push_back(uint32_t(page - base));
            scan.start = scan.walk_end;
        }
        return true;
    }
}
