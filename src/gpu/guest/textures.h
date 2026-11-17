#pragma once

// The textures the guest draws sample.

#include <cstdint>

namespace gpu
{
    struct DrawOp;
}

namespace gpu::guest
{
    // Before a draw's commit, which clears it: takes the device's dirty mask of the fetch constants,
    // so that the draw decodes the ones the title changed since the last draw. Drawing thread.
    void NoteChangedFetchConstants(uint32_t device);

    // Fills the textures of a draw from the device's fetch constants. A fetch constant is decoded
    // again only after NoteChangedFetchConstants saw it change, or when what it can sample changed
    // (a resolve, a destroyed texture, freed memory). A texture whose memory was written since its
    // last upload is uploaded first (posted before the draw). Memory is checked at the first use in
    // a frame, and again after the last UnlockResource of its header: by a check of its own
    // (CheckWrites) when it is new, was unlocked or was uploaded in the last kWrittenLatelyFrames
    // frames, by the frame's sweep (WrittenBySweep) otherwise. A write without an unlock that the
    // sweep answers for, made after the sweep, is uploaded in the next frame. Drawing thread, after
    // the draw's commit.
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
