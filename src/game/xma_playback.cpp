// XMAPlayback (the XDK library under XAudio's XMA source voices): the functions that drive the APU
// registers by hand, replaced by calls to the XMA hardware (src/apu/xma.h).
//
// The library keeps, per playback object, a stream count (+0), flags (+4) and an array of 96-byte
// streams (+8). A stream holds a 48-byte working copy of its hardware context (+0x00), the context
// address (+0x40), its hardware index (+0x50, 16 bits) and the guest addresses of its input buffers
// (+0x54, +0x58). Its registers are per-context bit masks: clear (0x7FEA1A80), lock (0x7FEA1A40),
// kick (0x7FEA1940), and a status word (0x7FEA1818). The recompiled stores to them only reach
// plain memory, so these four functions do the same work with the hardware calls instead. The
// fifth register user, XMAPlaybackWaitUntilModifyLockObtained, writes registers only after its
// query has failed for 1/8 s, which cannot happen here (see the query).

#include "apu/xma.h"
#include "kernel/kernel.h"

#include <cstring>

namespace
{
    constexpr uint32_t kStreamCount = 0x00;
    constexpr uint32_t kFlags = 0x04;
    constexpr uint32_t kStreams = 0x08;
    constexpr uint32_t kStreamSize = 96;
    constexpr uint32_t kStreamCopy = 0x00;
    constexpr uint32_t kStreamContext = 0x40;
    constexpr uint32_t kStreamIndex = 0x50;
    constexpr uint32_t kStreamInput0 = 0x54;
    constexpr uint32_t kStreamInput1 = 0x58;
    // The part of the hardware context the library copies: words 0-11.
    constexpr uint32_t kCopySize = 48;

    constexpr uint32_t kFlagLockRequested = 0x10000;
    constexpr uint32_t kFlagLockObtained = 0x20000;
    constexpr uint32_t kFlagAllocated = 0x40000;

    // Hardware context words and bits the query adjusts.
    constexpr uint32_t kPacketCountMask = 0xFFF;
    constexpr uint32_t kInputValidShift = 20;
    constexpr uint32_t kInput0Pointer = 0x14;
    constexpr uint32_t kInput1Pointer = 0x18;
    constexpr uint32_t kCurrentBufferWord = 0x10;
    constexpr uint32_t kCurrentBufferBit = 0x80000000;

    uint32_t Stream(uint32_t playback, uint32_t i)
    {
        return mem::Load32(playback + kStreams) + i * kStreamSize;
    }

    uint32_t StreamIndex(uint32_t stream)
    {
        return mem::Load16(stream + kStreamIndex);
    }

    // A stream without a hardware context has index 0 too, which belongs to another voice: the
    // register writes of the original would reach that context, so such streams are skipped.
    bool HasContext(uint32_t stream)
    {
        return mem::Load32(stream + kStreamContext) != 0;
    }
}

// XMAPlaybackAllocateContexts(playback): creates the hardware context of each stream that has none
// (XMACreateContext), records its index and clears it through the clear register.
PPC_FUNC(sub_82151FB8)
{
    uint32_t playback = ctx.r3.u32;
    uint32_t count = mem::Load32(playback + kStreamCount);
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t stream = Stream(playback, i);
        if (mem::Load32(stream + kStreamContext) != 0)
            continue;
        uint32_t context = apu::AllocateContext();
        if (context == 0)
        {
            LOG_ONCE("error: XMAPlaybackAllocateContexts: no free XMA context");
            ctx.r3.u64 = kernel::X_STATUS_NO_MEMORY;
            return;
        }
        uint32_t index = apu::ContextIndex(context);
        mem::Store32(stream + kStreamContext, context);
        mem::Store16(stream + kStreamIndex, uint16_t(index));
        apu::ClearContext(index);
    }
    mem::Store32(playback + kFlags, mem::Load32(playback + kFlags) | kFlagAllocated);
    ctx.r3.u64 = kernel::X_STATUS_SUCCESS;
}

// XMAPlaybackRequestModifyLock(playback): asks the hardware to stop on the streams' contexts
// through the lock register, unless a lock is already requested or obtained.
PPC_FUNC(sub_821521E0)
{
    uint32_t playback = ctx.r3.u32;
    uint32_t flags = mem::Load32(playback + kFlags);
    if ((flags & (kFlagLockRequested | kFlagLockObtained)) == 0)
    {
        uint32_t count = mem::Load32(playback + kStreamCount);
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t stream = Stream(playback, i);
            if (HasContext(stream))
                apu::DisableContext(StreamIndex(stream));
        }
    }
    mem::Store32(playback + kFlags, mem::Load32(playback + kFlags) | kFlagLockRequested);
    ctx.r3.u64 = 0;
}

// XMAPlaybackQueryModifyLockObtained(playback): 1 once the lock is held, after copying each
// context into its stream and dropping the buffers the hardware gave back. The original first
// returns 0 while the status register shows the hardware on one of the contexts; DisableContext
// returns only once the APU is off the context, so a requested lock is always obtained here.
PPC_FUNC(sub_82152328)
{
    uint32_t playback = ctx.r3.u32;
    uint32_t flags = mem::Load32(playback + kFlags);
    if ((flags & kFlagLockObtained) == 0)
    {
        uint32_t count = mem::Load32(playback + kStreamCount);
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t stream = Stream(playback, i);
            if (!HasContext(stream))
                continue;
            uint32_t copy = stream + kStreamCopy;
            memcpy(mem::Ptr(copy), mem::Ptr(mem::Load32(stream + kStreamContext)), kCopySize);
            uint32_t valid = (mem::Load32(copy) >> kInputValidShift) & 3;
            if ((valid & 1) == 0)
            {
                mem::Store32(copy, mem::Load32(copy) & ~kPacketCountMask);
                mem::Store32(copy + kInput0Pointer, 0);
                mem::Store32(stream + kStreamInput0, 0);
            }
            if ((valid & 2) == 0)
            {
                mem::Store32(copy + 4, mem::Load32(copy + 4) & ~kPacketCountMask);
                mem::Store32(copy + kInput1Pointer, 0);
                mem::Store32(stream + kStreamInput1, 0);
            }
            if (valid == 0)
                mem::Store32(copy + kCurrentBufferWord, mem::Load32(copy + kCurrentBufferWord) & ~kCurrentBufferBit);
        }
        mem::Store32(playback + kFlags, flags | kFlagLockObtained);
    }
    ctx.r3.u64 = 1;
}

// XMAPlaybackResumePlayback(playback): writes the working copies back when the lock is held, then
// restarts every stream's context through the kick register and drops the lock.
PPC_FUNC(sub_82152478)
{
    uint32_t playback = ctx.r3.u32;
    uint32_t count = mem::Load32(playback + kStreamCount);
    uint32_t flags = mem::Load32(playback + kFlags);
    if ((flags & kFlagLockObtained) != 0)
    {
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t stream = Stream(playback, i);
            if (HasContext(stream))
                memcpy(mem::Ptr(mem::Load32(stream + kStreamContext)), mem::Ptr(stream + kStreamCopy), kCopySize);
        }
    }
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t stream = Stream(playback, i);
        if (HasContext(stream))
            apu::EnableContext(StreamIndex(stream));
    }
    mem::Store32(playback + kFlags, flags & ~(kFlagLockRequested | kFlagLockObtained));
    ctx.r3.u64 = 0;
}
