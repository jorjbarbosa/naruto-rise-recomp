#pragma once

// Guest D3D resource headers as the renderer sees them.

#include "gpu/renderer/renderer.h"

#include <cstdint>
#include <optional>

namespace gpu::guest
{
    // GPU (physical) address of a guest address, converted as the D3D library does it.
    constexpr uint32_t GpuAddress(uint32_t address)
    {
        return (((address >> 20) + 0x200) & 0x1000) + (address & 0x1FFFFFFF);
    }

    // The guest address of GPU memory. Every physical allocation of the runtime comes from the
    // physical heap at 0xA0000000 (mem::Heap::Physical), where the GPU address is the offset.
    constexpr uint32_t GuestPhysical(uint32_t gpuAddress)
    {
        return 0xA0000000 + gpuAddress;
    }

    // The surface a guest surface header describes, or none when its format is not covered
    // (logged once per format).
    std::optional<SurfaceDesc> DescribeSurface(uint32_t header);

    // The parts of a texture fetch constant (6 dwords) the resolves and the present read.
    struct TextureFetch
    {
        uint32_t address = 0; // GPU address of the base level
        uint32_t format = 0;  // GPUTEXTUREFORMAT
        uint32_t width = 0;
        uint32_t height = 0;
        bool is2D = false;
    };

    TextureFetch ReadTextureFetch(uint32_t fetch);
}
