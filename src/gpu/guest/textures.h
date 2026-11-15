#pragma once

// The textures the guest draws sample.

#include <cstdint>

namespace gpu
{
    struct DrawOp;
}

namespace gpu::guest
{
    // Fills the textures of a draw from the device's fetch constants. A texture whose memory was
    // written since its last upload (mem::TakeWrittenPages) is uploaded first (posted before the
    // draw). Memory is checked at the first use in a frame, and again after the last UnlockResource
    // of its header. Render thread.
    void ReadDrawTextures(uint32_t device, DrawOp& op);

    // A resolve wrote the texture memory at a GPU address; draws sample the resolved texture there
    // until the destination header is destroyed.
    void NoteResolved(uint32_t address, uint32_t header);

    // A texture header is destroyed: the host textures of its memory are released.
    void ForgetTexture(uint32_t header);

    // Guest physical memory is freed: the host textures uploaded or resolved there are released.
    // The title can free texture memory without destroying the header over it. Any thread.
    void ForgetMemory(uint32_t address, uint32_t size);

    // The frame ended: every texture is checked again at its next use.
    void EndTextureFrame();
}
