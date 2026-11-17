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

    // A copy whose memory a check found written this many frames ago or less keeps its own check
    // (CheckWrites) at its first use in a frame, instead of the frame's sweep: memory the title
    // writes in every frame may be written after the sweep and read right after.
    constexpr uint64_t kWrittenLatelyFrames = 60;

    // Whether memory found written in the frame writtenFrame (0: never) counts as written lately in
    // the frame `frame`.
    constexpr bool WrittenLately(uint64_t writtenFrame, uint64_t frame)
    {
        return writtenFrame != 0 && writtenFrame + kWrittenLatelyFrames >= frame;
    }

    // A new write check of ranges (GPU addresses, all inside the physical heap): the pages written
    // since the last check that covered them get its number, check. Returns whether a page of the
    // ranges was found written after the check `since`. A copy of the memory made after the check
    // holds every write it found; later writes are found by later checks. The OS reports a written page
    // to one check only and copies share pages, so each page keeps the number of the last check that
    // found it written. Any thread.
    bool CheckWrites(const std::vector<std::pair<uint32_t, uint32_t>>& ranges, uint64_t since, uint64_t& check);

    // Whether the copy of the memory (GPU addresses, all inside the physical heap) has to check it
    // itself: a check up to the frame's sweep found a page of it written after the check `since`, or
    // the sweep did not cover it. The sweep is one write check, made by the first call of a frame,
    // of the span of the pages the calls of the previous frame asked about, so that the copies of
    // memory nobody writes need no check of their own. False means no write up to the sweep; a write
    // after it is found by the next frame's sweep or by a CheckWrites of the memory. Any thread.
    bool WrittenBySweep(uint32_t address, uint32_t bytes, uint64_t since);
    bool WrittenBySweep(const std::vector<std::pair<uint32_t, uint32_t>>& ranges, uint64_t since);

    // The frame ended: the next WrittenBySweep sweeps again.
    void EndWatchFrame();

    // The number of the latest write check (0: none yet). Any thread.
    uint64_t LatestCheck();

    // The number of the latest check that found a page of [address, address + bytes) written (0: none),
    // from what the checks recorded, without asking the OS. Any thread.
    uint64_t LastWriteFound(uint32_t address, uint32_t bytes);
}
