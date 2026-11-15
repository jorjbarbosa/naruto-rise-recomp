#pragma once

// Guest addresses and D3D::CDevice offsets of the XDK 6274 D3D library in default.xex, named in one
// place. Names come from the XDK library match (EngineSrc build/xenonmatch) where there is one.

#include <cstdint>

// Hooked functions: the symbols of the recompiled functions. A definition with PPC_FUNC(<name>)
// replaces every call to them (they are weak aliases of their __imp__ implementation).
#define D3D_INITIALIZE_RING sub_8212FA80          // D3D::CDevice ring setup, inside device creation
#define D3D_INITIALIZE_XPS sub_821480D0           // D3D::InitializeXps
#define D3D_KICK_OFF sub_8212F608                 // D3D::CDevice::KickOff
#define D3D_START_NEW_SEGMENT sub_8212EBB0        // D3D::CDevice::StartNewSegment
#define D3D_ALLOCATE_FROM_RING sub_8212EA18       // ring space after the segment (BeginVertices...)
#define D3D_BLOCK_ON_FENCE sub_8212F708           // D3DDevice_BlockOnFence
#define D3D_ADD_COMMANDS_TO_PRIMARY sub_8212EE38  // D3D::CDevice::AddCommandsToPrimaryBuffer
#define D3D_ADD_CALLS_TO_PRIMARY sub_8212EF28     // D3D::CDevice::AddCallsToPrimaryBuffer
#define D3D_QUEUE_INDIRECT_BUFFER sub_8212F530    // D3D::CDevice::QueueIndirectBuffer

namespace gpu::guest
{
    // D3D::CDevice fields (byte offsets from the device).
    namespace dev
    {
        // Segment of the command ring being written. +48 points at the last dword written (packets
        // go at +48 + 4); a writer that finds +48 above the soft limit +56 calls KickOff first.
        // The soft limit sits 160 bytes below the segment end +52.
        constexpr uint32_t kCursor = 48;
        constexpr uint32_t kSegmentEnd = 52;
        constexpr uint32_t kSegmentLimit = 56;
        constexpr uint32_t kSegmentMargin = 160;

        // Writeback block (96 bytes): +0 is the last completed fence, written by the GPU.
        constexpr uint32_t kWriteback = 10896;
        // Scratch block (32 bytes) that the library's GPU packets address.
        constexpr uint32_t kScratch = 10900;
        // Fence of the open batch. KickOff closes the batch with it and adds 2.
        constexpr uint32_t kNextFence = 10908;
        // Initial value of kNextFence (the ring setup stores it when the field is 0).
        constexpr uint32_t kFirstFence = 3;

        // Flags byte. Bit 0x80: a command buffer is being recorded, and kicks are not submitted.
        constexpr uint32_t kFlags10940 = 10940;
        constexpr uint8_t kFlagRecording = 0x80;

        // Nonzero while kicks are held back: KickOff then queues the segment in a list at +13352
        // instead of submitting it, and BlockOnFence returns at once for the open batch.
        constexpr uint32_t kKickHold = 13216;

        // Presentation interval of the frames, in 60 Hz vblanks (D3DPRESENT_INTERVAL_*).
        constexpr uint32_t kPresentInterval = 13580;

        // Fence of the previous frame, set by D3DDevice_Swap; StartNewSegment waits for it.
        constexpr uint32_t kFrameFence = 14924;

        // Creation flags. Bit 0: StartNewSegment does not wait for kFrameFence.
        constexpr uint32_t kCreateFlags = 22280;
        constexpr uint32_t kCreateNoSegmentWait = 1;
    }
}
