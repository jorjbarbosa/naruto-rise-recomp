#pragma once

// Entry points of the GPU layer for the kernel (src/kernel/video.cpp).

#include <cstdint>

namespace gpu
{
    class Renderer;

    // VdInitializeEngines, called by the device creation: creates the renderer on the game window.
    void InitializeEngines();
    // VdSwap, called by D3DDevice_Swap with the front buffer's texture fetch constant (6 dwords):
    // presents the texture resolved at its address and paces the game at its presentation interval.
    void Swap(uint32_t frontBufferFetch);

    // MmFreePhysicalMemory, before the memory is freed: releases the host textures of that memory.
    void FreePhysicalMemory(uint32_t address, uint32_t size);

    // Null before InitializeEngines or when the renderer could not be created.
    Renderer* ActiveRenderer();
}
