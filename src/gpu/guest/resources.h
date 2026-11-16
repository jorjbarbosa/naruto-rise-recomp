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

    // A vertex fetch constant (2 dwords, in the device or in a D3DVertexBuffer): the buffer's GPU address,
    // its size and its endian swap. The address dword holds the fetch type in bits 0..1; the size dword
    // holds the endian swap in bits 0..1 and the size in dwords in bits 2..25.
    struct VertexFetch
    {
        uint32_t address = 0;
        uint32_t bytes = 0;
        uint32_t endian = 0;
    };

    constexpr VertexFetch DecodeVertexFetch(uint32_t address, uint32_t size)
    {
        return { GpuAddress(address & ~3u), ((size >> 2) & 0xFFFFFF) * 4, size & 3 };
    }

    // GPU endian swaps (fetch constants, index buffers).
    constexpr uint32_t kEndian8in16 = 1;
    constexpr uint32_t kEndian8in32 = 2;
    constexpr uint32_t kEndian16in32 = 3;

    // A dword as the GPU reads it with an endian swap, from its bytes in guest memory loaded in host
    // order.
    constexpr uint32_t SwapEndian32(uint32_t value, uint32_t endian)
    {
        switch (endian)
        {
        case kEndian8in16: return ((value & 0x00FF00FF) << 8) | ((value >> 8) & 0x00FF00FF);
        case kEndian8in32: return __builtin_bswap32(value);
        case kEndian16in32: return (value << 16) | (value >> 16);
        default: return value;
        }
    }

    // How the GPU reads the 16-bit indices of an index buffer with an endian swap: it swaps whole
    // dwords, so 8in32 and 16in32 also exchange the two indices of each dword (the index at byte
    // address a is read from a ^ exchange), and 8in16 and 8in32 swap the bytes of each index.
    struct Index16Swap
    {
        uint32_t exchange = 0;
        bool swapBytes = false;
    };

    constexpr Index16Swap DecodeIndex16Swap(uint32_t endian)
    {
        return { endian == kEndian8in32 || endian == kEndian16in32 ? 2u : 0u,
            endian == kEndian8in16 || endian == kEndian8in32 };
    }

    // The guest address of GPU memory. Every physical allocation of the runtime comes from the
    // physical heap at 0xA0000000 (mem::Heap::Physical), where the GPU address is the offset.
    constexpr uint32_t GuestPhysical(uint32_t gpuAddress)
    {
        return 0xA0000000 + gpuAddress;
    }

    // The data of a D3DIndexBuffer header: GPU address and size in bytes.
    struct IndexData
    {
        uint32_t address = 0;
        uint32_t bytes = 0;
    };

    // The data range of an index buffer header; none when the data is not in physical memory (no data
    // yet), where GpuAddress and GuestPhysical are not inverses.
    std::optional<IndexData> ReadIndexData(uint32_t header);

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
