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

    // Packets the render thread finished, since the start: drawn, the rest of its frame run and the
    // device lock released.
    uint64_t PacketsDrawn();

    // Waits until the render thread drew every packet handed so far. False when the timeout passed
    // first. Call it from the thread that hands the packets (the main thread), or the count can grow
    // while it waits.
    bool WaitForPacketsDrawn(std::chrono::milliseconds timeout);

    // Holds the render thread before it starts the next packet (before it takes the device lock),
    // until ReleaseDrawing or for at most kDrawingHoldLimit (then it goes on and logs once). The purity probe uses it so the render
    // thread's own writes to game memory stay out of a comparison. Main thread.
    constexpr std::chrono::milliseconds kDrawingHoldLimit{ 1000 };
    void HoldDrawing();
    void ReleaseDrawing();
}
