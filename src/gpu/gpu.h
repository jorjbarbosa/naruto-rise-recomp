#pragma once

// Entry points of the GPU layer for the kernel (src/kernel/video.cpp).

#include <cstdint>

namespace gpu
{
    class Renderer;

    // VdInitializeEngines, called by the device creation: creates the renderer on the game window.
    void InitializeEngines();
    // VdSwap, called by D3DDevice_Swap with the front buffer's texture fetch constant (6 dwords):
    // presents the texture resolved at its address and paces the game at its presentation interval,
    // or at the rate set by SetPaceRate.
    void Swap(uint32_t frontBufferFetch);

    // The rate in frames per second the Swap paces at instead of the title's presentation interval;
    // 0 gives the interval back. Set by the game's clock when it decides the frame rate (state
    // interpolation); any thread, read by the next Swap.
    void SetPaceRate(uint32_t rate);

    // MmFreePhysicalMemory, before the memory is freed: releases the host textures of that memory.
    void FreePhysicalMemory(uint32_t address, uint32_t size);

    // Null before InitializeEngines or when the renderer could not be created.
    Renderer* ActiveRenderer();
}
