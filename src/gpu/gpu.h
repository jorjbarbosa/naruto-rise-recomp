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

    // The narrowest wide picture, so that a window a few pixels off 16:9 stays 16:9, and the widest:
    // 32:9, twice the guest's 16:9.
    constexpr float kMinWideStretch = 1.01f;
    constexpr float kMaxWideStretch = 2.0f;
    // How many times wider than its own 16:9 the guest's frame is shown: the window's shape over 16:9
    // from kMinWideStretch to kMaxWideStretch, and 1 for a narrower window, no window or the setting
    // video.ultrawide off. The guest always renders 1280x720: the Swap stretches the front buffer by
    // this, and the game squeezes what it draws by the same amount (game/widescreen.cpp). Any thread.
    float WideStretch();
    // The vertical scale of the internal resolution the frames are drawn at, over the guest's 1280x720
    // (Renderer::VerticalScale), as of the last Swap; 1 before the first and without a renderer. It
    // changes once a change of the window or of the setting has settled. Any thread.
    float RenderScale();
    // A span of a viewport's width, as shares of it from its left side.
    struct DrawClip
    {
        float left = 0.0f;
        float right = 1.0f;

        bool operator==(const DrawClip&) const = default;
    };
    // Clips the draws the calling thread makes from now on to a span of their viewport's width (the
    // whole of it at the start), on top of the title's own scissor. For the interface items the game
    // squeezes into a 16:9 frame of a wide picture (game/widescreen.cpp), whose art runs past that frame.
    void SetDrawClip(const DrawClip& clip);

    // MmFreePhysicalMemory, before the memory is freed: releases the host textures of that memory.
    void FreePhysicalMemory(uint32_t address, uint32_t size);

    // Null before InitializeEngines or when the renderer could not be created.
    Renderer* ActiveRenderer();
}
