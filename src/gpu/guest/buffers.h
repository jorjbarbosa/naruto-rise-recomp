#pragma once

// Vertex and index buffers the host keeps a copy of, so that draws reference it instead of copying
// their vertices and indices.

#include "gpu/renderer/renderer.h"

#include <cstdint>

namespace gpu::guest
{
    // Whether a draw can read a guest buffer (key: GPU address and size of the whole buffer, and the
    // format of its host copy) from the host's copy. True when the host has a copy that holds the
    // memory as it is now, or one is posted before the caller's draw. False when the memory was written
    // in this frame (dynamic vertices or indices): the draw copies what it reads. The memory is checked
    // for writes at the buffer's first use in a frame and after InvalidateBuffers, as the textures are:
    // by the frame's sweep (WrittenBySweep) for a host copy whose memory was not written in the last
    // kWrittenLatelyFrames frames, by a check of its own (CheckWrites) otherwise. A write without an
    // unlock that the sweep answers for, made after the sweep, is seen in the next frame. A buffer gets
    // a host copy once it went from a check of its own in one frame to one in a later frame without
    // writes. Render thread.
    bool UseHostedBuffer(const BufferKey& key);

    // Guest physical memory at the GPU addresses [begin, end) is freed: the host copies of the buffers
    // that overlap it go. Any thread.
    void ForgetBuffers(uint32_t begin, uint32_t end);

    // A vertex or index buffer over the GPU addresses [begin, end) was unlocked: the buffers there are
    // checked again at their next use. Any thread.
    void InvalidateBuffers(uint32_t begin, uint32_t end);

    // The frame ended.
    void EndBufferFrame();
}
