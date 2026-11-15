#pragma once

// Vertex buffers the host keeps a copy of, so that draws reference it instead of copying their
// vertices.

#include <cstdint>

namespace gpu::guest
{
    // Whether a draw can read the vertex buffer at [address, address + bytes) (GPU addresses, the whole
    // buffer of the fetch constant) from the host's copy. True when the host has a copy that holds the
    // memory as it is now, or one is posted before the caller's draw. False when the memory was written
    // in this frame (dynamic vertices): the draw copies its vertices. The memory is checked for writes
    // (mem::TakeWrittenPages) at the buffer's first use in a frame and after InvalidateBuffers, as the
    // textures are; a buffer gets a host copy once it went from a check in one frame to a check in a
    // later one without writes. Render thread.
    bool UseHostedBuffer(uint32_t address, uint32_t bytes);

    // Guest physical memory at the GPU addresses [begin, end) is freed: the host copies of the buffers
    // that overlap it go. Any thread.
    void ForgetBuffers(uint32_t begin, uint32_t end);

    // A vertex buffer over the GPU addresses [begin, end) was unlocked: the buffers there are checked
    // again at their next use. Any thread.
    void InvalidateBuffers(uint32_t begin, uint32_t end);

    // The frame ended.
    void EndBufferFrame();
}
