#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace gpu::guest
{
    // The guest D3D device (D3D::CDevice of the XDK library) seen from the host.
    //
    // There is no command ring. Every packet the library writes goes into a discard segment that
    // nothing reads; when it fills up, KickOff starts it over. The fence protocol of the console is
    // kept: KickOff closes the open batch with the fence in +10908 and adds 2, and the host
    // publishes each fence into the writeback block once the renderer has finished the work posted
    // before it, as the GPU would.
    class Device
    {
    public:
        // The ring setup of the device creation: writeback and scratch blocks, discard segment and
        // inline area instead of the ring.
        void InitializeArena(uint32_t device);
        uint32_t Address() const { return device_.load(std::memory_order_acquire); }

        // Points the segment (+48, +52, +56) at the start of the discard buffer.
        void ResetSegment();
        // Space the library places after the segment (vertices of BeginVertices, ...). Nothing
        // consumes it yet, so it is reused in a circle.
        uint32_t AllocateInline(uint32_t bytes, uint32_t alignment);

        // Closes the open batch; returns its fence.
        uint32_t Kick();
        // D3DDevice_BlockOnFence: returns once the fence has completed. The open batch is kicked
        // first, as the original does.
        void BlockOnFence(uint32_t fence);
        // The work posted before the fence has completed (renderer completion thread).
        void Publish(uint32_t fence);

    private:
        uint32_t KickLocked(uint32_t device);
        uint32_t Completed() const;

        std::atomic<uint32_t> device_{ 0 };
        uint32_t writeback_ = 0;
        uint32_t discard_ = 0;
        uint32_t inline_ = 0;

        std::mutex fenceMutex_;
        std::condition_variable fenceCv_;

        std::mutex inlineMutex_;
        uint32_t inlineCursor_ = 0;
    };

    Device& GetDevice();
}
