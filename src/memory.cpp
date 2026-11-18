// Guest memory: the reservation of the guest space, its heaps and atomic access (memory.h).

#include "memory.h"

#include "host/address_space.h"

#include <atomic>
#include <map>
#include <mutex>
#include <vector>

namespace mem
{
    static_assert(kPageSize == host::kWritePageSize, "the guest's write watch pages are the host's");

    bool Init()
    {
        g_base = host::ReserveAddressSpace(kSize);
        return g_base != nullptr;
    }

    void Commit(uint32_t address, uint32_t size)
    {
        host::CommitAddressSpace(address, size);
    }

    bool TakeWrittenPages(uint32_t address, uint32_t size, std::vector<uint32_t>& pages)
    {
        return host::TakeWrittenPages(address, size, pages);
    }

    uint32_t AtomicLoad32(uint32_t a)
    {
        std::atomic_ref<uint32_t> value(*reinterpret_cast<uint32_t*>(g_base + a));
        return __builtin_bswap32(value.load(std::memory_order_acquire));
    }

    int32_t AtomicAdd32(uint32_t a, int32_t delta)
    {
        std::atomic_ref<uint32_t> value(*reinterpret_cast<uint32_t*>(g_base + a));
        uint32_t old = value.load();
        uint32_t desired;
        do
        {
            desired = __builtin_bswap32(uint32_t(int32_t(__builtin_bswap32(old)) + delta));
        } while (!value.compare_exchange_weak(old, desired));
        return int32_t(__builtin_bswap32(desired));
    }

    bool AtomicCas32(uint32_t a, uint32_t expected, uint32_t desired)
    {
        std::atomic_ref<uint32_t> value(*reinterpret_cast<uint32_t*>(g_base + a));
        uint32_t e = __builtin_bswap32(expected);
        return value.compare_exchange_strong(e, __builtin_bswap32(desired));
    }

    // First-fit allocator over a guest range. Free blocks are kept zeroed, so allocations never
    // need clearing (fresh guest memory is zero too).
    class RangeAllocator
    {
    public:
        RangeAllocator(uint32_t begin, uint32_t end)
        {
            m_free[begin] = end - begin;
        }

        uint32_t Alloc(uint32_t size, uint32_t alignment)
        {
            if (size == 0)
                size = 1;
            if (alignment < 16)
                alignment = 16;
            size = (size + 15) & ~15u;

            std::lock_guard lock(m_mutex);
            for (auto it = m_free.begin(); it != m_free.end(); ++it)
            {
                uint64_t blockBase = it->first;
                uint64_t blockEnd = blockBase + it->second;
                uint64_t start = (blockBase + alignment - 1) & ~uint64_t(alignment - 1);
                if (start + size > blockEnd)
                    continue;

                m_free.erase(it);
                if (start > blockBase)
                    m_free[uint32_t(blockBase)] = uint32_t(start - blockBase);
                if (start + size < blockEnd)
                    m_free[uint32_t(start + size)] = uint32_t(blockEnd - start - size);
                m_used[uint32_t(start)] = size;
                return uint32_t(start);
            }
            return 0;
        }

        bool Free(uint32_t address)
        {
            std::lock_guard lock(m_mutex);
            auto it = m_used.find(address);
            if (it == m_used.end())
                return false;

            uint32_t base = it->first;
            uint32_t size = it->second;
            m_used.erase(it);
            memset(g_base + base, 0, size);

            auto next = m_free.lower_bound(base);
            if (next != m_free.end() && base + size == next->first)
            {
                size += next->second;
                next = m_free.erase(next);
            }
            if (next != m_free.begin())
            {
                auto prev = std::prev(next);
                if (prev->first + prev->second == base)
                {
                    prev->second += size;
                    return true;
                }
            }
            m_free[base] = size;
            return true;
        }

        bool Query(uint32_t address, uint32_t* base, uint32_t* size)
        {
            std::lock_guard lock(m_mutex);
            auto it = m_used.upper_bound(address);
            if (it == m_used.begin())
                return false;
            --it;
            if (address >= it->first + it->second)
                return false;
            *base = it->first;
            *size = it->second;
            return true;
        }

    private:
        std::mutex m_mutex;
        std::map<uint32_t, uint32_t> m_free;
        std::map<uint32_t, uint32_t> m_used;
    };

    static RangeAllocator g_system(kSystemHeapBegin, kSystemHeapEnd);
    static RangeAllocator g_virtual(kVirtualHeapBegin, kVirtualHeapEnd);
    static RangeAllocator g_stack(kStackBegin, kStackEnd);
    static RangeAllocator g_physical(kPhysicalBegin, kPhysicalEnd);

    static RangeAllocator& HeapFor(Heap heap)
    {
        switch (heap)
        {
        case Heap::System: return g_system;
        case Heap::Virtual: return g_virtual;
        case Heap::Stack: return g_stack;
        default: return g_physical;
        }
    }

    static RangeAllocator* HeapForAddress(uint32_t address)
    {
        if (address >= kSystemHeapBegin && address < kSystemHeapEnd) return &g_system;
        if (address >= kVirtualHeapBegin && address < kVirtualHeapEnd) return &g_virtual;
        if (address >= kStackBegin && address < kStackEnd) return &g_stack;
        if (address >= kPhysicalBegin && address < kPhysicalEnd) return &g_physical;
        return nullptr;
    }

    uint32_t Alloc(Heap heap, uint32_t size, uint32_t alignment)
    {
        return HeapFor(heap).Alloc(size, alignment);
    }

    bool Free(uint32_t address)
    {
        auto* heap = HeapForAddress(address);
        return heap != nullptr && heap->Free(address);
    }

    bool Query(uint32_t address, uint32_t* base, uint32_t* size)
    {
        auto* heap = HeapForAddress(address);
        return heap != nullptr && heap->Query(address, base, size);
    }

    const char* String(uint32_t address)
    {
        return address != 0 ? reinterpret_cast<const char*>(g_base + address) : "(null)";
    }
}
