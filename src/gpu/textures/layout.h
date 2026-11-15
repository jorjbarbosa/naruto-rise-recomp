#pragma once

// Guest texture layout: the fields of a texture fetch constant and where each level and face of
// the texture lies in GPU memory.

#include <array>
#include <cstdint>
#include <vector>

namespace gpu::textures
{
    struct Format;

    // How a guest texture is sampled. 1D textures are sampled as 2D textures one texel high.
    enum class Dimension : uint8_t
    {
        k1D,
        k2D,
        k3D,
        kCube,
    };

    // A texture fetch constant (xe_gpu_texture_fetch_t), dwords in host order.
    struct Fetch
    {
        std::array<uint32_t, 6> d{};

        uint32_t Type() const { return d[0] & 3; } // 2: texture
        uint32_t Signs() const { return (d[0] >> 2) & 0xFF; } // 2 bits per component
        uint32_t Clamp() const { return (d[0] >> 10) & 0x1FF; } // 3 bits each for X, Y, Z
        uint32_t PitchDiv32() const { return (d[0] >> 22) & 0x1FF; }
        bool Tiled() const { return (d[0] >> 31) != 0; }
        uint32_t Format() const { return d[1] & 0x3F; }
        uint32_t Endian() const { return (d[1] >> 6) & 3; }
        bool Stacked() const { return ((d[1] >> 10) & 1) != 0; }
        uint32_t BaseAddress() const { return d[1] & 0xFFFFF000; }
        uint32_t Width() const { return (d[2] & 0x1FFF) + 1; } // 2D and cube
        uint32_t Height() const { return ((d[2] >> 13) & 0x1FFF) + 1; }
        uint32_t Swizzle() const { return (d[3] >> 1) & 0xFFF; } // 3 bits per component: x y z w 0 1
        int32_t ExpAdjust() const { return int32_t(d[3] << 13) >> 26; }
        uint32_t Filter() const { return (d[3] >> 19) & 0x1FF; } // mag, min, mip (2 bits each), aniso (3)
        uint32_t MipFilter() const { return (d[3] >> 23) & 3; } // 2: base map only
        bool Border() const { return (d[3] >> 31) != 0; } // a one-texel border on every side
        uint32_t MipMin() const { return (d[4] >> 2) & 0xF; }
        uint32_t MipMax() const { return (d[4] >> 6) & 0xF; }
        int32_t LodBias() const { return int32_t(d[4] << 10) >> 22; } // in 1/32
        uint32_t BorderColor() const { return d[5] & 3; }
        uint32_t DimensionField() const { return (d[5] >> 9) & 3; } // 0 1D, 1 2D, 2 3D, 3 cube
        bool PackedMips() const { return ((d[5] >> 11) & 1) != 0; }
        uint32_t MipAddress() const { return d[5] & 0xFFFFF000; }
    };

    constexpr uint32_t kFetchTypeTexture = 2;

    // Where one level lies: its place (the base, a mip or a packed mip tail) and its offset in it.
    struct LevelPlace
    {
        uint32_t address = 0;    // GPU address of the place
        uint32_t rowPitch = 0;   // bytes between rows of blocks
        uint32_t x0 = 0;         // the level's offset in a packed mip tail, in blocks
        uint32_t y0 = 0;
        uint32_t extent = 0;     // bytes of one face the level may read from address
        uint32_t faceStride = 0; // bytes between the faces of a cube
    };

    // A guest texture as the renderer creates and fills it.
    struct Layout
    {
        Dimension dimension = Dimension::k2D;
        uint32_t format = 0; // GPUTEXTUREFORMAT
        uint32_t endian = 0;
        bool tiled = false;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t faces = 1;
        uint32_t mipMin = 0; // the levels the fetch samples, clamped to the memory it has
        uint32_t mipMax = 0;
        bool hasBase = true; // false: level 0 is not in memory (mipMin is at least 1)
        std::vector<LevelPlace> levels; // mipMax + 1
    };

    // The layout of the texture of a fetch constant (GPU addresses), or null with the reason when
    // it is not covered: 3D, stacked and bordered textures, formats outside formats.h, and sizes the
    // host cannot create.
    bool DescribeTexture(const Fetch& fetch, Layout& layout, const char*& reason);

    // The ranges of GPU memory a layout reads, merged and sorted: (address, bytes).
    std::vector<std::pair<uint32_t, uint32_t>> MemoryRanges(const Layout& layout);
}
