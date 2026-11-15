// Virtual, physical and pool memory.

#include "kernel/kernel.h"

using namespace kernel;

namespace
{
    constexpr uint32_t MEM_COMMIT = 0x1000;
    constexpr uint32_t MEM_RESERVE = 0x2000;
    constexpr uint32_t MEM_DECOMMIT = 0x4000;
    constexpr uint32_t MEM_RELEASE = 0x8000;
    constexpr uint32_t MEM_PRIVATE = 0x20000;
    constexpr uint32_t MEM_LARGE_PAGES = 0x20000000;
    constexpr uint32_t MEM_16MB_PAGES = 0x80000000;
    constexpr uint32_t PAGE_READWRITE = 0x04;

    uint32_t PageSize(uint32_t flags)
    {
        if (flags & MEM_16MB_PAGES)
            return 16 * 1024 * 1024;
        if (flags & MEM_LARGE_PAGES)
            return 64 * 1024;
        return 4096;
    }
}

// NtAllocateVirtualMemory(baseAddress*, regionSize*, allocationType, protect, debugMemory)
GUEST_FUNC(NtAllocateVirtualMemory)
{
    uint32_t basePtr = ctx.r3.u32;
    uint32_t sizePtr = ctx.r4.u32;
    uint32_t type = ctx.r5.u32;
    uint32_t protect = ctx.r6.u32;

    uint32_t address = mem::Load32(basePtr);
    uint32_t pageSize = PageSize(type | protect);
    uint32_t size = (mem::Load32(sizePtr) + pageSize - 1) & ~(pageSize - 1);

    if (address != 0)
    {
        // Committing (part of) an earlier reservation: everything is committed already.
        uint32_t allocBase, allocated;
        if (!mem::Query(address, &allocBase, &allocated))
        {
            Log("warning: NtAllocateVirtualMemory at unknown address %08X", address);
            ctx.r3.u64 = X_STATUS_INVALID_PARAMETER;
            return;
        }
    }
    else
    {
        address = mem::Alloc(mem::Heap::Virtual, size, std::max<uint32_t>(pageSize, 0x10000));
        if (address == 0)
        {
            Log("warning: NtAllocateVirtualMemory out of memory (%08X bytes)", size);
            ctx.r3.u64 = X_STATUS_NO_MEMORY;
            return;
        }
    }

    TRACE("NtAllocateVirtualMemory: %08X bytes at %08X (type %08X)", size, address, type);
    mem::Store32(basePtr, address);
    mem::Store32(sizePtr, size);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// NtFreeVirtualMemory(baseAddress*, regionSize*, freeType, debugMemory)
GUEST_FUNC(NtFreeVirtualMemory)
{
    uint32_t address = mem::Load32(ctx.r3.u32);
    if (ctx.r5.u32 & MEM_RELEASE)
    {
        uint32_t allocBase, size;
        if (mem::Query(address, &allocBase, &size))
        {
            mem::Free(allocBase);
            if (ctx.r4.u32 != 0)
                mem::Store32(ctx.r4.u32, size);
        }
    }
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// NtQueryVirtualMemory(address, MEMORY_BASIC_INFORMATION*, ...)
GUEST_FUNC(NtQueryVirtualMemory)
{
    uint32_t address = ctx.r3.u32;
    uint32_t info = ctx.r4.u32;
    uint32_t allocBase, size;
    bool known = mem::Query(address, &allocBase, &size);
    if (!known)
    {
        allocBase = address & ~0xFFFu;
        size = 0x1000;
    }
    mem::Store32(info + 0x00, allocBase);
    mem::Store32(info + 0x04, allocBase);
    mem::Store32(info + 0x08, PAGE_READWRITE);
    mem::Store32(info + 0x0C, size);
    mem::Store32(info + 0x10, known ? MEM_COMMIT : 0x10000 /* MEM_FREE */);
    mem::Store32(info + 0x14, known ? PAGE_READWRITE : 0);
    mem::Store32(info + 0x18, MEM_PRIVATE);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// MmAllocatePhysicalMemoryEx(flags, size, protect, minAddress, maxAddress, alignment) -> address
GUEST_FUNC(MmAllocatePhysicalMemoryEx)
{
    uint32_t size = ctx.r4.u32;
    uint32_t protect = ctx.r5.u32;
    uint32_t alignment = ctx.r8.u32;
    uint32_t pageSize = PageSize(protect);
    size = (size + pageSize - 1) & ~(pageSize - 1);
    alignment = std::max(alignment, pageSize);

    uint32_t address = mem::Alloc(mem::Heap::Physical, size, alignment);
    TRACE("MmAllocatePhysicalMemoryEx: %08X bytes at %08X (protect %08X)", size, address, protect);
    if (address == 0)
        Log("warning: MmAllocatePhysicalMemoryEx out of memory (%08X bytes)", size);
    ctx.r3.u64 = address;
}

// MmFreePhysicalMemory(type, address)
GUEST_FUNC(MmFreePhysicalMemory)
{
    mem::Free(ctx.r4.u32);
}

GUEST_FUNC(MmGetPhysicalAddress)
{
    ctx.r3.u64 = ctx.r3.u32 & 0x1FFFFFFF;
}

GUEST_FUNC(MmQueryAddressProtect)
{
    ctx.r3.u64 = PAGE_READWRITE;
}

GUEST_FUNC(MmSetAddressProtect) {}

GUEST_FUNC(MmQueryAllocationSize)
{
    uint32_t allocBase, size;
    ctx.r3.u64 = mem::Query(ctx.r3.u32, &allocBase, &size) ? size : 0;
}

// ExAllocatePoolTypeWithTag(size, tag, poolType)
GUEST_FUNC(ExAllocatePoolTypeWithTag)
{
    ctx.r3.u64 = mem::Alloc(mem::Heap::System, ctx.r3.u32);
}

GUEST_FUNC(ExAllocatePool)
{
    ctx.r3.u64 = mem::Alloc(mem::Heap::System, ctx.r3.u32);
}

GUEST_FUNC(ExFreePool)
{
    mem::Free(ctx.r3.u32);
}
