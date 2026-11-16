#pragma once

// The packets the main thread hands to the Jade render thread and the packets that thread drew.
//
// The main thread fills a packet (the frame's lists) and hands it over with JADE_HAND_PACKET, which
// first waits until the render thread is idle, so at most one handed packet is being drawn. The
// render thread still reads the game objects the packet names, so the main thread must not destroy
// them before it is drawn (state interpolation plan, F1). Any thread.

#include <chrono>
#include <cstdint>

namespace game
{
    // Packets handed to the render thread to be drawn, since the start.
    uint64_t PacketsHanded();

    // Packets whose draw (JADE_DRAW_PACKET) returned, since the start: the render thread no longer
    // reads the game objects they name. The rest of its frame (fences, the swap) follows.
    uint64_t PacketsRead();

    // Packets the render thread finished, since the start: drawn, the rest of its frame run and the
    // device lock released.
    uint64_t PacketsDrawn();

    // Waits until the render thread drew every packet handed so far. False when the timeout passed
    // first. Call it from the thread that hands the packets (the main thread), or the count can grow
    // while it waits.
    bool WaitForPacketsDrawn(std::chrono::milliseconds timeout);

    // Waits until the render thread read every packet handed so far (PacketsRead), including the one of
    // a hand-off in progress. False when the timeout passed first. Any thread but the render thread.
    bool WaitForPacketsRead(std::chrono::milliseconds timeout);

    // Whether the calling thread is the one that draws the packets (known from its first packet).
    bool OnRenderThread();

    // Whether a packet the main thread handed (one with the world's objects) is not read yet, or the
    // main thread is handing one. The loading screen's thread hands its own packets during loads;
    // since every hand-off first waits until the render thread is idle, a packet from that thread
    // means the main thread's last one was read.
    bool WorldPacketPending();

    // Holds the render thread before it starts the next packet (before it takes the device lock),
    // until ReleaseDrawing or for at most kDrawingHoldLimit (then it goes on and logs once). The purity probe uses it so the render
    // thread's own writes to game memory stay out of a comparison. Main thread.
    constexpr std::chrono::milliseconds kDrawingHoldLimit{ 1000 };
    void HoldDrawing();
    void ReleaseDrawing();
}
