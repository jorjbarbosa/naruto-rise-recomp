// Counts of the packets handed to the Jade render thread and drawn by it, and the hold of its drawing
// (render_packets.h).

#include "game/render_packets.h"

#include "game/addresses.h"
#include "kernel/kernel.h"

#include <atomic>
#include <condition_variable>
#include <mutex>

DECLARE_ORIGINAL(JADE_HAND_PACKET);
DECLARE_ORIGINAL(JADE_DRAW_PACKET);
DECLARE_ORIGINAL(JADE_DEVICE_LOCK);
DECLARE_ORIGINAL(JADE_DEVICE_UNLOCK);

namespace game
{
    namespace
    {
        // The device lock: +8 its recursion count.
        constexpr uint32_t kLockCount = 8;

        std::atomic<uint64_t> g_handed{ 0 };
        std::atomic<uint64_t> g_drawn{ 0 };
        std::atomic<bool> g_hold{ false };
        std::atomic<kernel::GuestThread*> g_renderThread{ nullptr }; // the thread that draws the packets
        std::mutex g_mutex; // pairs with g_drawnChanged and g_holdChanged
        std::condition_variable g_drawnChanged;
        std::condition_variable g_holdChanged;

        // On the render thread: a packet is being drawn, from the draw until the device lock is
        // released at the end of the render loop's iteration.
        thread_local bool t_drawing = false;

        // Render thread: waits while the drawing is held.
        void WaitWhileHeld()
        {
            std::unique_lock lock(g_mutex);
            if (!g_holdChanged.wait_for(lock, kDrawingHoldLimit, [] { return !g_hold.load(std::memory_order_acquire); }))
                LOG_ONCE("render packets: the render thread was held for %lld ms and draws anyway",
                    (long long)kDrawingHoldLimit.count());
        }
    }

    uint64_t PacketsHanded()
    {
        return g_handed.load(std::memory_order_acquire);
    }

    uint64_t PacketsDrawn()
    {
        return g_drawn.load(std::memory_order_acquire);
    }

    bool WaitForPacketsDrawn(std::chrono::milliseconds timeout)
    {
        uint64_t handed = PacketsHanded();
        std::unique_lock lock(g_mutex);
        return g_drawnChanged.wait_for(lock, timeout, [&] { return PacketsDrawn() >= handed; });
    }

    void HoldDrawing()
    {
        g_hold.store(true, std::memory_order_release);
    }

    void ReleaseDrawing()
    {
        {
            std::lock_guard lock(g_mutex);
            g_hold.store(false, std::memory_order_release);
        }
        g_holdChanged.notify_all();
    }
}

// Hand-off of a packet (main thread, and the loading screen's thread): waits until the render thread
// is idle, flips the packet index, stores whether the packet is to be drawn in kPacketPending, wakes
// the render thread and waits until it read that word. Hooked to count the packets to be drawn; the
// word is not written again before the next hand-off.
PPC_FUNC(JADE_HAND_PACKET)
{
    ORIGINAL(JADE_HAND_PACKET)(ctx, base);
    if (mem::Load32(game::jade::kPacketPending) == 1)
        game::g_handed.fetch_add(1, std::memory_order_acq_rel);
}

// Draw of a handed packet, on the render thread (its only caller is the render thread's loop, which
// takes the device lock before it and runs more of the frame, 820F0A80 and 820EED48, before it
// releases the lock). Hooked to know the render thread and to mark the drawing.
PPC_FUNC(JADE_DRAW_PACKET)
{
    game::g_renderThread.store(kernel::CurrentThread(), std::memory_order_release);
    game::t_drawing = true;
    ORIGINAL(JADE_DRAW_PACKET)(ctx, base);
}

// The D3D device lock (every thread that touches the device). Hooked to hold the render thread
// before it starts a packet (HoldDrawing): the outermost lock of its loop's iteration.
PPC_FUNC(JADE_DEVICE_LOCK)
{
    if (game::g_hold.load(std::memory_order_acquire) && !game::t_drawing &&
        kernel::CurrentThread() == game::g_renderThread.load(std::memory_order_acquire))
        game::WaitWhileHeld();
    ORIGINAL(JADE_DEVICE_LOCK)(ctx, base);
}

// The release of the device lock. Hooked to count a packet as drawn when the render thread releases
// the lock it held for the whole iteration: from then on it only waits for the next packet.
PPC_FUNC(JADE_DEVICE_UNLOCK)
{
    // Read before the release: the count is this thread's while it owns the lock, and another thread
    // can take the lock right after.
    bool outermost = game::t_drawing && mem::Load32(ctx.r3.u32 + game::kLockCount) == 1;
    ORIGINAL(JADE_DEVICE_UNLOCK)(ctx, base);
    if (outermost)
    {
        game::t_drawing = false;
        {
            std::lock_guard guard(game::g_mutex);
            game::g_drawn.fetch_add(1, std::memory_order_acq_rel);
        }
        game::g_drawnChanged.notify_all();
    }
}
