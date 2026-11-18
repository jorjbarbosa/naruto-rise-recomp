// The copy of a texture level from CPU rows, shared by the guest textures and the UI's.

#include "gpu/renderer/level_copy.h"

#include "gpu/renderer/context.h"

#include <cstring>

namespace gpu
{
    namespace
    {
        // Placed footprints: rows aligned to 256 bytes, subresources to 512.
        constexpr uint32_t kRowAlignment = 256;
        constexpr uint32_t kPlacementAlignment = 512;
    }

    void CopyLevel(Context& context, plume::RenderCommandList& list, plume::RenderTexture& texture, uint32_t level,
        uint32_t face, plume::RenderFormat format, uint32_t width, uint32_t height, const LevelRows& rows)
    {
        uint32_t pitch = (rows.rowBytes + kRowAlignment - 1) / kRowAlignment * kRowAlignment;
        Context::Upload staging = context.Allocate(uint64_t(pitch) * rows.rowCount, kPlacementAlignment);
        for (uint32_t row = 0; row < rows.rowCount; row++)
            memcpy(staging.data + uint64_t(row) * pitch, rows.data + size_t(row) * rows.rowBytes, rows.rowBytes);
        // The footprint's row length in texels.
        uint32_t rowWidth = pitch / rows.unitBytes * rows.unitTexels;
        list.copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(&texture, level, face),
            plume::RenderTextureCopyLocation::PlacedFootprint(staging.buffer, format, width, height, 1, rowWidth,
                staging.offset));
    }
}
