#pragma once

#include "gpu/renderer/renderer.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>

namespace gpu::guest
{
    // The guest D3D device (D3D::CDevice of the XDK library) seen from the host.
    //
    // There is no command ring. Every packet the library writes goes into a discard segment that
    // nothing reads; when it fills up, KickOff starts it over. The fence protocol of the console is
    // kept: KickOff closes the open batch with the fence in +10908 and adds 2, and the host
    // publishes each fence into the writeback block once the renderer has finished the work posted
    // before it, as the GPU would.
    //
    // Tiling: on the console a tiled pass renders into EDRAM surfaces smaller than the screen and
    // replays its commands once per tile. The host has no tiles and no replay: while a pass is
    // tiled, every bound surface smaller than the tiled area is replaced by a backing surface of the
    // whole area, which clears, draws and the per-tile resolves all use.
    class Device
    {
    public:
        // A backing surface has the key of its guest surface with this bit set (headers are aligned).
        static constexpr uint32_t kBackingKey = 1;
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

        // A GPU register (reg::) from the register blocks of the device, as the draws commit them.
        uint32_t Register(uint32_t index) const;
        float RegisterFloat(uint32_t index) const;

        // The pass between BeginTiling and EndTiling, of the given extent. Render thread only.
        void BeginTiling(uint32_t width, uint32_t height);
        void EndTiling();
        // The host surface of a bound slot (dev::kRenderTargets + 4 i or dev::kDepthStencil): the
        // backing while the pass is tiled. None when the slot is empty or its format is not covered.
        // Render thread only.
        std::optional<SurfaceDesc> BoundSurface(uint32_t slot) const;

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

        bool tiling_ = false;
        uint32_t tilingWidth_ = 0;
        uint32_t tilingHeight_ = 0;
    };

    // GPU float bits as a float.
    float LoadFloat(uint32_t address);

    Device& GetDevice();
}
