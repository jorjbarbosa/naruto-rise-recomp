#pragma once

// Entry points of the GPU layer for the kernel (src/kernel/video.cpp).

#include <cstdint>

namespace gpu
{
    class Renderer;

    // VdInitializeEngines, called by the device creation: creates the renderer on the game window.
    void InitializeEngines();
    // VdSwap, called by D3DDevice_Swap with the front buffer's texture fetch constant (6 dwords):
    // presents the frame and paces the game at its presentation interval.
    void Swap(uint32_t frontBufferFetch);

    // Null before InitializeEngines or when the renderer could not be created.
    Renderer* ActiveRenderer();
}
