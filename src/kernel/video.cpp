// Video driver (Vd*). The engines and the flip go to the GPU layer (src/gpu); the rest succeeds and
// the ring buffer setup is recorded.

#include "gpu/gpu.h"
#include "kernel/kernel.h"

#include <atomic>
#include <cstring>

using namespace kernel;

namespace
{
    struct VideoState
    {
        uint32_t interruptCallback = 0;
        uint32_t interruptUserData = 0;
        uint32_t ringBuffer = 0;
        uint32_t ringBufferSizeLog2 = 0;
        uint32_t readPointerWriteBack = 0;
    } g_video;

    std::atomic<uint64_t> g_frames{ 0 };

    uint32_t FloatBits(float value)
    {
        uint32_t bits;
        memcpy(&bits, &value, sizeof(bits));
        return bits;
    }
}

namespace kernel
{
    // X_VIDEO_MODE, shared with XGetVideoMode.
    void StoreVideoMode(uint32_t mode)
    {
        memset(mem::Ptr(mode), 0, 48);
        mem::Store32(mode + 0x00, 1280);
        mem::Store32(mode + 0x04, 720);
        mem::Store32(mode + 0x08, 0); // interlaced
        mem::Store32(mode + 0x0C, 1); // widescreen
        mem::Store32(mode + 0x10, 1); // high definition
        mem::Store32(mode + 0x14, FloatBits(60.0f));
        mem::Store32(mode + 0x18, 1); // NTSC
        mem::Store32(mode + 0x1C, 0x4A);
        mem::Store32(mode + 0x20, 0x01);
    }
}

GUEST_FUNC(VdQueryVideoMode) { StoreVideoMode(ctx.r3.u32); }
GUEST_FUNC(VdQueryVideoFlags) { ctx.r3.u64 = 3; } // widescreen | high definition

GUEST_FUNC(VdGetCurrentDisplayInformation)
{
    uint32_t info = ctx.r3.u32;
    memset(mem::Ptr(info), 0, 0x58);
    mem::Store16(info + 0x00, 1280);
    mem::Store16(info + 0x02, 720);
    mem::Store16(info + 0x48, 1280);
    mem::Store16(info + 0x4A, 720);
}

// VdGetCurrentDisplayGamma(type*, power*)
GUEST_FUNC(VdGetCurrentDisplayGamma)
{
    mem::Store32(ctx.r3.u32, 1);
    mem::Store32(ctx.r4.u32, FloatBits(2.22222233f));
}

GUEST_FUNC(VdInitializeEngines)
{
    gpu::InitializeEngines();
    ctx.r3.u64 = 1;
}

GUEST_FUNC(VdShutdownEngines) {}

// VdSetGraphicsInterruptCallback(callback, userData)
GUEST_FUNC(VdSetGraphicsInterruptCallback)
{
    g_video.interruptCallback = ctx.r3.u32;
    g_video.interruptUserData = ctx.r4.u32;
    Log("video: interrupt callback %08X (user data %08X)", ctx.r3.u32, ctx.r4.u32);
}

// VdInitializeRingBuffer(address, sizeLog2)
GUEST_FUNC(VdInitializeRingBuffer)
{
    g_video.ringBuffer = ctx.r3.u32;
    g_video.ringBufferSizeLog2 = ctx.r4.u32;
    Log("video: ring buffer at %08X, 2^%u bytes", ctx.r3.u32, ctx.r4.u32);
}

// VdEnableRingBufferRPtrWriteBack(address, blockSizeLog2)
GUEST_FUNC(VdEnableRingBufferRPtrWriteBack)
{
    g_video.readPointerWriteBack = ctx.r3.u32;
    Log("video: read pointer write-back at %08X", ctx.r3.u32);
}

GUEST_FUNC(VdGetSystemCommandBuffer)
{
    memset(mem::Ptr(ctx.r3.u32), 0, 0x94);
    mem::Store32(ctx.r3.u32, 0xBEEF0000);
    mem::Store32(ctx.r4.u32, 0xBEEF0001);
}

GUEST_FUNC(VdSetSystemCommandBufferGpuIdentifierAddress) {}
GUEST_FUNC(VdIsHSIOTrainingSucceeded) { ctx.r3.u64 = 1; }
GUEST_FUNC(VdRetrainEDRAM) { ctx.r3.u64 = 0; }
GUEST_FUNC(VdRetrainEDRAMWorker) { ctx.r3.u64 = 0; }
GUEST_FUNC(VdEnableDisableClockGating) {}
GUEST_FUNC(VdCallGraphicsNotificationRoutines) { ctx.r3.u64 = 0; }
GUEST_FUNC(VdSetDisplayMode) { ctx.r3.u64 = 0; }

// VdPersistDisplay(unknown, block*): the block is later released with MmFreePhysicalMemory.
GUEST_FUNC(VdPersistDisplay)
{
    if (ctx.r4.u32 != 0)
        mem::Store32(ctx.r4.u32, mem::Alloc(mem::Heap::Physical, 64, 32));
    ctx.r3.u64 = 1;
}

// VdInitializeScalerCommandBuffer(sourceXY, sourceWH, outputXY, outputWH, unknown, verticalFilter,
//     verticalParams, horizontalFilter, horizontalParams, unknown, destination, count):
// filled with PM4 NOPs. Destination and count are the 11th and 12th arguments (D3D::SetupScaler
// stores them at r1+0x64 and r1+0x6C).
GUEST_FUNC(VdInitializeScalerCommandBuffer)
{
    uint32_t destination = StackArg32(ctx, 10);
    uint32_t count = StackArg32(ctx, 11);
    for (uint32_t i = 0; i < count; i++)
        mem::Store32(destination + i * 4, 0x80000000);
    ctx.r3.u64 = count;
}

// VdSwap(buffer, frontBufferFetch, gpuIdentifier, systemCommandBuffer, ..., frontBase*, format*,
//     colorSpace*): writes the flip packets into buffer (part of the command segment, which nothing
// reads). The host presents here instead.
GUEST_FUNC(VdSwap)
{
    uint64_t frame = ++g_frames;
    if (frame == 1 || frame % 600 == 0)
        Log("video: VdSwap, frame %llu", (unsigned long long)frame);
    gpu::Swap(ctx.r4.u32);
    ctx.r3.u64 = 0;
}
