#pragma once

// Write checks of GPU memory: which pages of the physical heap the CPU (or the OS) wrote since a
// previous check, shared by the host copies of guest memory (textures, vertex buffers).

#include <cstdint>
#include <utility>
#include <vector>

namespace gpu::guest
{
    // GPU memory pages (mem::kPageSize) of the physical heap.
    constexpr uint32_t kPageShift = 12;
    constexpr uint32_t kPhysicalPages = 0x20000000 >> kPageShift;
    constexpr uint64_t kPhysicalEnd = uint64_t(kPhysicalPages) << kPageShift;

    // A new write check of ranges (GPU addresses, all inside the physical heap): the pages written
    // since the last check that covered them get its number, check. Returns whether a page of the
    // ranges was found written after the check `since`. A copy of the memory made after the check
    // holds every write it found; later writes are found by later checks. The OS reports a written page
    // to one check only and copies share pages, so each page keeps the number of the last check that
    // found it written. Any thread.
    bool CheckWrites(const std::vector<std::pair<uint32_t, uint32_t>>& ranges, uint64_t since, uint64_t& check);

    // The number of the latest write check (0: none yet). Any thread.
    uint64_t LatestCheck();

    // The number of the latest check that found a page of [address, address + bytes) written (0: none),
    // from what the checks recorded, without asking the OS. Any thread.
    uint64_t LastWriteFound(uint32_t address, uint32_t bytes);
}
