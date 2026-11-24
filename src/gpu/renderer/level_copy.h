#pragma once

#include <plume_render_interface.h>

#include <cstdint>

namespace gpu
{
    class Context;

    // One level (or face) of a texture's content: rows of units, a unit being a texel or, for a
    // block-compressed format, a block of unitTexels x unitTexels texels.
    struct LevelRows
    {
        const uint8_t* data = nullptr;
        uint32_t rowBytes = 0;   // bytes of one row, with no padding
        uint32_t rowCount = 0;
        uint32_t unitTexels = 1; // texels along a unit's side
        uint32_t unitBytes = 4;  // bytes of a unit
    };

    // Records the copy of the rows into the texture's subresource (level, face) of the given texel
    // size, through upload memory placed as D3D12 needs it. The texture must be in COPY_DEST.
    void CopyLevel(Context& context, plume::RenderCommandList& list, plume::RenderTexture& texture, uint32_t level,
        uint32_t face, plume::RenderFormat format, uint32_t width, uint32_t height, const LevelRows& rows);
}
