#pragma once

// The picture in a window wider than 16:9 (widescreen.cpp).

namespace game::widescreen
{
    // A packet was handed to the render thread and the next one starts in the other packet's memory:
    // forgets the draws marked there two packets ago, and takes the window's shape for the packet
    // that starts. Call it right after the hand-off, on the thread that made it.
    void PacketHanded();
}
