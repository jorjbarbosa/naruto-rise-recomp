#pragma once

#include <cstdint>
#include <cstring>

// Guest memory: the whole 32-bit Xbox 360 address space, reserved up front and committed on first
// touch. Guest values are big-endian.
//
// Address map used by the runtime:
//   0x30000000-0x3FFFFFFF  system heap (kernel objects, pools, XAM allocations)
//   0x40000000-0x6FFFFFFF  virtual memory (NtAllocateVirtualMemory)
//   0x70000000-0x7EFFFFFF  thread stacks
//   0x7FC80000-...         GPU registers (plain memory, nothing listens yet)
//   0x82000000-0x82C5FFFF  default.xex image
//   0x82C60000-0x8387FFFF  its indirect call table (see PPC_LOOKUP_FUNC and ppc_module.h)
//   0x88000000-0x883BFFFF  AI2C.dll image, loaded by the game (XexLoadImage)
//   0x883C0000-0x88A8183F  its indirect call table
//   0xA0000000-0xBFFFFFFF  physical memory (MmAllocatePhysicalMemoryEx)
namespace mem
{
    inline uint8_t* g_base = nullptr;
    constexpr uint64_t kSize = 0x100000000ull;

    bool Init();

    // Commits the pages of a guest range. Guest pages are otherwise committed on first touch through
    // an access violation, but host OS calls (ReadFile, WriteFile...) fail with ERROR_NOACCESS on
    // reserved pages instead of faulting, so call this before handing them guest memory.
    void Commit(uint32_t address, uint32_t size);

    inline uint8_t* Ptr(uint32_t address) { return g_base + address; }
    inline bool IsHostInGuest(const void* p) { return p >= g_base && p < g_base + kSize; }

    inline uint8_t Load8(uint32_t a) { return g_base[a]; }
    inline uint16_t Load16(uint32_t a) { uint16_t v; memcpy(&v, g_base + a, 2); return __builtin_bswap16(v); }
    inline uint32_t Load32(uint32_t a) { uint32_t v; memcpy(&v, g_base + a, 4); return __builtin_bswap32(v); }
    inline uint64_t Load64(uint32_t a) { uint64_t v; memcpy(&v, g_base + a, 8); return __builtin_bswap64(v); }
    inline void Store8(uint32_t a, uint8_t v) { g_base[a] = v; }
    inline void Store16(uint32_t a, uint16_t v) { v = __builtin_bswap16(v); memcpy(g_base + a, &v, 2); }
    inline void Store32(uint32_t a, uint32_t v) { v = __builtin_bswap32(v); memcpy(g_base + a, &v, 4); }
    inline void Store64(uint32_t a, uint64_t v) { v = __builtin_bswap64(v); memcpy(g_base + a, &v, 8); }

    // Atomic (acquire) load of a big-endian 32-bit guest value, for host code that waits for a store
    // made by another thread.
    uint32_t AtomicLoad32(uint32_t a);
    // Atomic add on a big-endian 32-bit guest value; returns the new value.
    int32_t AtomicAdd32(uint32_t a, int32_t delta);
    // Atomic compare-exchange on a big-endian 32-bit guest value.
    bool AtomicCas32(uint32_t a, uint32_t expected, uint32_t desired);

    enum class Heap
    {
        System,
        Virtual,
        Stack,
        Physical,
    };

    // Returns 0 when out of space. Fresh memory is always zeroed.
    uint32_t Alloc(Heap heap, uint32_t size, uint32_t alignment = 16);
    bool Free(uint32_t address);
    // Finds the allocation containing address.
    bool Query(uint32_t address, uint32_t* base, uint32_t* size);

    // Copies a NUL-terminated guest string.
    const char* String(uint32_t address);
}
