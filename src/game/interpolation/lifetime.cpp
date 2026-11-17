// The lifetime reports for the transforms (lifetime.h).

#include "game/interpolation/lifetime.h"

#include "memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>

namespace game::interpolation::lifetime
{
    namespace
    {
        // The heap's chunks (82188C88 reads them so): the block is the chunk plus 8; the chunk's head
        // word, at block - 4, holds its size with two flag bits, bit 1 set while it is in use (the
        // heap's free does nothing without it, mapped chunks included). An in-use chunk also owns the
        // next chunk's first word, so the block ends 4 bytes past the chunk.
        constexpr uint32_t kHeadOffset = 4;
        constexpr uint32_t kChunkOffset = 8;
        constexpr uint32_t kSizeMask = ~3u;
        constexpr uint32_t kInUse = 2;
        constexpr uint32_t kMinChunk = 16;
        constexpr uint32_t kMaxChunk = 0x10000000;
        // Reports kept between two drains before they are lost.
        constexpr size_t kMaxRanges = 1 << 16;
        // The watched pages: 64 KB each, marked while something in them is tracked.
        constexpr uint32_t kPageShift = 16;
        constexpr uint32_t kPages = uint32_t(mem::kSize >> kPageShift);

        std::unique_ptr<std::array<std::atomic<uint8_t>, kPages>> g_pages = std::make_unique<std::array<std::atomic<uint8_t>, kPages>>();
        std::atomic<bool> g_watching = false; // a page is marked
        std::mutex g_mutex;                   // guards g_ranges and g_lost
        std::vector<Range> g_ranges;
        bool g_lost = false;
        std::atomic<uint64_t> g_badHeaders = 0, g_overflows = 0;

        // Whether a block's chunk lies in the system, virtual or physical heap's region.
        bool InHeap(uint32_t block)
        {
            return (block >= mem::kSystemHeapBegin + kChunkOffset && block < mem::kVirtualHeapEnd)
                || (block >= mem::kPhysicalBegin + kChunkOffset && block < mem::kPhysicalEnd);
        }

        bool Watched(Range range)
        {
            uint64_t last = (uint64_t(range.end) - 1) >> kPageShift;
            for (uint64_t page = range.begin >> kPageShift; page <= last; page++)
                if ((*g_pages)[page].load(std::memory_order_relaxed) != 0)
                    return true;
            return false;
        }

        void Lose()
        {
            std::lock_guard lock(g_mutex);
            g_lost = true;
        }

        void Report(Range range)
        {
            if (range.end <= range.begin || !Watched(range))
                return;
            std::lock_guard lock(g_mutex);
            if (g_ranges.size() < kMaxRanges)
                g_ranges.push_back(range);
            else
                g_lost = true;
        }
    }

    void Watch(uint32_t begin, uint32_t end)
    {
        if (end <= begin)
            return;
        uint32_t last = (end - 1) >> kPageShift;
        for (uint32_t page = begin >> kPageShift; page <= last; page++)
            (*g_pages)[page].store(1, std::memory_order_relaxed);
        g_watching.store(true);
    }

    void Unwatch()
    {
        g_watching.store(false);
        for (auto& page : *g_pages)
            page.store(0, std::memory_order_relaxed);
        std::lock_guard lock(g_mutex);
        g_ranges.clear();
        g_lost = false;
    }

    void NoteFree(uint32_t block)
    {
        // The header is read only inside the heaps' regions (the heap's free reads it only above its
        // least address).
        if (!g_watching.load(std::memory_order_relaxed) || !InHeap(block))
            return;
        uint32_t head = mem::Load32(block - kHeadOffset);
        // A block not in use is not freed (the heap's free returns at once).
        if ((head & kInUse) == 0)
            return;
        uint32_t size = head & kSizeMask;
        if (size < kMinChunk || size > kMaxChunk)
        {
            // Its extent is unknown: the tracker must drop everything.
            g_badHeaders.fetch_add(1, std::memory_order_relaxed);
            Lose();
            return;
        }
        uint64_t end = uint64_t(block) - kChunkOffset + size + kHeadOffset;
        Report({ block, uint32_t(std::min<uint64_t>(end, 0xFFFFFFFFull)) });
    }

    void NoteDestroy(uint32_t object)
    {
        if (object != 0 && g_watching.load(std::memory_order_relaxed))
            Report({ object, object + 1 });
    }

    void Drain(std::vector<Range>& ranges, bool& everything)
    {
        ranges.clear();
        std::lock_guard lock(g_mutex);
        ranges.swap(g_ranges);
        everything = g_lost;
        if (g_lost)
            g_overflows.fetch_add(1, std::memory_order_relaxed);
        g_lost = false;
    }

    Totals Current()
    {
        return { g_badHeaders.load(std::memory_order_relaxed), g_overflows.load(std::memory_order_relaxed) };
    }
}
