// The guest device without a command ring: discard segment, inline area and the fence protocol,
// with the hooks on the ring functions of the D3D library.

#include "gpu/guest/device.h"

#include "gpu/gpu.h"
#include "gpu/guest/addresses.h"
#include "gpu/renderer/renderer.h"
#include "kernel/kernel.h"

#include <chrono>

namespace gpu::guest
{
    namespace
    {
        constexpr uint32_t kWritebackBytes = 96;
        constexpr uint32_t kScratchBytes = 32;
        // The discard segment. Writers check the soft limit before a packet and may write past it
        // up to the segment end; the slack after the end absorbs a writer that overshoots.
        constexpr uint32_t kDiscardBytes = 0x100000;
        constexpr uint32_t kDiscardSlack = 0x10000;
        constexpr uint32_t kInlineBytes = 0x100000;
        constexpr auto kFenceReport = std::chrono::seconds(5);

        Device g_device;

        // Fence order as the console compares fences: relative to the next fence, a smaller
        // distance is a later fence.
        bool Done(uint32_t next, uint32_t fence, uint32_t completed)
        {
            return next - fence >= next - completed;
        }
    }

    Device& GetDevice()
    {
        return g_device;
    }

    void Device::InitializeArena(uint32_t device)
    {
        writeback_ = mem::Alloc(mem::Heap::System, kWritebackBytes, 32);
        uint32_t scratch = mem::Alloc(mem::Heap::System, kScratchBytes, 32);
        discard_ = mem::Alloc(mem::Heap::System, kDiscardBytes + kDiscardSlack, 0x1000);
        inline_ = mem::Alloc(mem::Heap::System, kInlineBytes, 0x1000);
        if (writeback_ == 0 || scratch == 0 || discard_ == 0 || inline_ == 0)
            Fatal("gpu: out of system memory for the device arena");

        mem::Store32(device + dev::kWriteback, writeback_);
        mem::Store32(device + dev::kScratch, scratch);
        if (mem::Load32(device + dev::kNextFence) == 0)
            mem::Store32(device + dev::kNextFence, dev::kFirstFence);
        mem::Store32(writeback_, mem::Load32(device + dev::kNextFence) - 2);
        device_.store(device, std::memory_order_release);
        ResetSegment();
        Log("gpu: device %08X: no command ring, discard segment at %08X", device, discard_);
    }

    void Device::ResetSegment()
    {
        uint32_t device = Address();
        uint32_t end = discard_ + kDiscardBytes;
        mem::Store32(device + dev::kCursor, discard_ - 4);
        mem::Store32(device + dev::kSegmentEnd, end);
        mem::Store32(device + dev::kSegmentLimit, end - dev::kSegmentMargin);
    }

    uint32_t Device::AllocateInline(uint32_t bytes, uint32_t alignment)
    {
        if (bytes > kInlineBytes)
            Fatal("gpu: ring allocation of %u bytes is larger than the inline area", bytes);
        alignment = std::max(alignment, 4u);
        std::lock_guard lock(inlineMutex_);
        uint32_t at = (inlineCursor_ + alignment - 1) & ~(alignment - 1);
        if (at + bytes > kInlineBytes)
            at = 0;
        inlineCursor_ = at + bytes;
        return inline_ + at;
    }

    uint32_t Device::Completed() const
    {
        return mem::Load32(writeback_);
    }

    uint32_t Device::KickLocked(uint32_t device)
    {
        uint32_t fence = mem::Load32(device + dev::kNextFence);
        mem::Store32(device + dev::kNextFence, fence + 2);
        if (Renderer* renderer = ActiveRenderer())
            renderer->Kick(fence);
        else
            mem::Store32(writeback_, fence); // nothing to wait for
        return fence;
    }

    uint32_t Device::Kick()
    {
        std::lock_guard lock(fenceMutex_);
        return KickLocked(Address());
    }

    void Device::BlockOnFence(uint32_t fence)
    {
        uint32_t device = Address();
        std::unique_lock lock(fenceMutex_);
        uint32_t next = mem::Load32(device + dev::kNextFence);
        if (Done(next, fence, Completed()))
            return;
        if (fence == next)
        {
            if (mem::Load32(device + dev::kKickHold) != 0)
                return; // as the original: the open batch cannot be kicked now
            KickLocked(device);
        }

        auto report = std::chrono::steady_clock::now() + kFenceReport;
        while (!Done(mem::Load32(device + dev::kNextFence), fence, Completed()))
        {
            if (fenceCv_.wait_until(lock, report) == std::cv_status::timeout)
            {
                Log("gpu: still waiting for fence %u (completed %u, next %u)", fence, Completed(),
                    mem::Load32(device + dev::kNextFence));
                report += kFenceReport;
            }
        }
    }

    void Device::Publish(uint32_t fence)
    {
        {
            std::lock_guard lock(fenceMutex_);
            uint32_t next = mem::Load32(Address() + dev::kNextFence);
            if (!Done(next, fence, Completed()))
                mem::Store32(writeback_, fence);
        }
        fenceCv_.notify_all();
    }
}

using namespace gpu::guest;

// D3D::CDevice::KickOff(device): closes the segment, submits it as a batch and returns the cursor of
// the next segment. Here the batch is closed for the fence protocol and the segment starts over.
PPC_FUNC(D3D_KICK_OFF)
{
    uint32_t device = ctx.r3.u32;
    if (mem::Load32(device + dev::kKickHold) == 0 && (mem::Load8(device + dev::kFlags10940) & dev::kFlagRecording) == 0)
        GetDevice().Kick();
    GetDevice().ResetSegment();
    ctx.r3.u64 = mem::Load32(device + dev::kCursor);
}

// D3D::CDevice::StartNewSegment(device, dwords): a new segment with room for the given dwords, after
// the previous frame's fence unless the device was created without that wait. Returns its start.
PPC_FUNC(D3D_START_NEW_SEGMENT)
{
    uint32_t device = ctx.r3.u32;
    uint64_t bytes = uint64_t(ctx.r4.u32) * 4;
    // Past the slack, the writer would overwrite whatever follows the discard buffer.
    if (bytes > gpu::guest::kDiscardBytes + gpu::guest::kDiscardSlack)
        Fatal("gpu: StartNewSegment of %u dwords is larger than the discard segment", ctx.r4.u32);
    if (bytes > gpu::guest::kDiscardBytes)
        LOG_ONCE("gpu: StartNewSegment of %u dwords reaches into the discard slack", ctx.r4.u32);
    GetDevice().ResetSegment();
    uint32_t frameFence = mem::Load32(device + dev::kFrameFence);
    if ((mem::Load32(device + dev::kCreateFlags) & dev::kCreateNoSegmentWait) == 0 && frameFence != 0)
    {
        GetDevice().BlockOnFence(frameFence);
        mem::Store32(device + dev::kFrameFence, 0);
    }
    ctx.r3.u64 = mem::Load32(device + dev::kCursor) + 4;
}

// (device, dwords, alignment): space after the segment, returned aligned.
PPC_FUNC(D3D_ALLOCATE_FROM_RING)
{
    ctx.r3.u64 = GetDevice().AllocateInline(ctx.r4.u32 * 4, ctx.r5.u32);
}

// D3DDevice_BlockOnFence(device, fence, reason).
PPC_FUNC(D3D_BLOCK_ON_FENCE)
{
    GetDevice().BlockOnFence(ctx.r4.u32);
}

// The functions below feed the hardware ring directly. Without a ring they have nothing to do.
PPC_FUNC(D3D_ADD_COMMANDS_TO_PRIMARY)
{
    LOG_ONCE("gpu: AddCommandsToPrimaryBuffer skipped (no command ring), lr %08llX", (unsigned long long)ctx.lr);
}

PPC_FUNC(D3D_ADD_CALLS_TO_PRIMARY)
{
    LOG_ONCE("gpu: AddCallsToPrimaryBuffer skipped (no command ring), lr %08llX", (unsigned long long)ctx.lr);
}

// (device, cursor, ...): returns the cursor after the packets it adds; nothing is added.
PPC_FUNC(D3D_QUEUE_INDIRECT_BUFFER)
{
    LOG_ONCE("gpu: QueueIndirectBuffer skipped (no command ring), lr %08llX", (unsigned long long)ctx.lr);
    ctx.r3.u64 = ctx.r4.u64;
}
