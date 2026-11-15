// Guest texture layout: levels, mip tails and faces of a texture in GPU memory, as the Xenos
// texture fetch addresses them.

#include "gpu/textures/layout.h"

#include "gpu/textures/formats.h"

#include <algorithm>
#include <bit>

namespace gpu::textures
{
    namespace
    {
        // Xenos texture memory: 32x32-block tiles, linear mip rows aligned to 256 bytes, every
        // level and face aligned to 4 KB.
        constexpr uint32_t kTile = 32;
        constexpr uint32_t kLinearRowAlignment = 256;
        constexpr uint32_t kSubresourceAlignment = 4096;
        // The sizes the host creates: 2D textures up to 8192 texels (the guest maximum), 1D ones up
        // to the host's 2D width.
        constexpr uint32_t kMaxSize2D = 8192;
        constexpr uint32_t kMaxSize1D = 16384;
        // A texture past this many bytes of memory is not plausible.
        constexpr uint32_t kMaxBytes = 64 << 20;
        constexpr uint32_t kGpuMemory = 0x20000000;
        constexpr uint32_t kNone = ~0u;

        uint32_t Align(uint32_t value, uint32_t alignment)
        {
            return (value + alignment - 1) / alignment * alignment;
        }

        uint32_t Log2Floor(uint32_t value)
        {
            return uint32_t(std::bit_width(value)) - 1;
        }

        uint32_t Log2Ceil(uint32_t value)
        {
            return uint32_t(std::bit_width(value - 1));
        }

        uint32_t NextPow2(uint32_t value)
        {
            return std::bit_ceil(value);
        }

        // The first level of the packed mip tail: where the shorter side reaches 16 texels.
        uint32_t PackedMipLevel(uint32_t width, uint32_t height)
        {
            uint32_t log2 = Log2Ceil(std::min(width, height));
            return log2 > 4 ? log2 - 4 : 0;
        }

        // The texel offset of a level in the packed mip tail tile. The three largest levels sit
        // along the shorter axis (16, 8, 4 texels from the origin); the smaller ones along the
        // longer axis, halving each level.
        void PackedMipOffset(uint32_t width, uint32_t height, uint32_t level, uint32_t& x, uint32_t& y)
        {
            uint32_t log2Width = Log2Ceil(width);
            uint32_t log2Height = Log2Ceil(height);
            uint32_t log2Size = std::min(log2Width, log2Height);
            x = 0;
            y = 0;
            if (log2Size > 4 + level)
                return;
            uint32_t packedBase = log2Size > 4 ? log2Size - 4 : 0;
            uint32_t packed = level - packedBase;
            bool wide = log2Width > log2Height;
            if (packed < 3)
            {
                if (wide)
                    y = 16 >> packed;
                else
                    x = 16 >> packed;
            }
            else if (wide)
            {
                x = (1u << (log2Width - packedBase)) >> (packed - 2);
            }
            else
            {
                y = (1u << (log2Height - packedBase)) >> (packed - 2);
            }
        }

        // The highest byte a tiled region of right x bottom blocks reaches: the origin of the tile
        // holding its last block plus the span of the tiled address within a tile.
        uint32_t TiledExtent(uint32_t right, uint32_t bottom, uint32_t pitchBlocks, uint32_t log2Bytes)
        {
            uint32_t origin = TiledOffset2D((right - 1) & ~(kTile - 1), (bottom - 1) & ~(kTile - 1), pitchBlocks, log2Bytes);
            switch (log2Bytes)
            {
            case 0: return origin + 0xA00;
            case 1: return origin + 0xC00;
            default: return origin + (0x400u << log2Bytes);
            }
        }

        // The pitches of a stored level: the base takes the pitch of the fetch constant, the mips
        // those of a mip of the base rounded up to powers of two.
        struct Stored
        {
            uint32_t rowPitch = 0;
            uint32_t pitchBlocks = 0;
            uint32_t faceStride = 0;
        };

        Stored StoredPitches(const Fetch& fetch, const Format& format, const Layout& layout, bool base, uint32_t level)
        {
            uint32_t rowTexels = base ? fetch.PitchDiv32() * 32 : std::max(NextPow2(layout.width) >> level, 1u);
            uint32_t rows = base ? layout.height : std::max(NextPow2(layout.height) >> level, 1u);
            Stored stored;
            stored.pitchBlocks = Align((rowTexels + format.block - 1) / format.block, kTile);
            stored.rowPitch = stored.pitchBlocks * format.bytesPerBlock;
            if (!layout.tiled && !base)
                stored.rowPitch = Align(stored.rowPitch, kLinearRowAlignment);
            uint32_t sliceRows = layout.dimension == Dimension::k1D ? 1 : Align((rows + format.block - 1) / format.block, kTile);
            stored.faceStride = Align(stored.rowPitch * sliceRows, kSubresourceAlignment);
            return stored;
        }
    }

    bool DescribeTexture(const Fetch& fetch, Layout& layout, const char*& reason)
    {
        layout = {};
        switch (fetch.DimensionField())
        {
        case 0: layout.dimension = Dimension::k1D; break;
        case 1: layout.dimension = Dimension::k2D; break;
        case 2: reason = "3D textures are not covered"; return false;
        default: layout.dimension = Dimension::kCube; break;
        }
        if (layout.dimension == Dimension::k2D && fetch.Stacked())
        {
            reason = "stacked textures are not covered";
            return false;
        }
        if (fetch.Border())
        {
            reason = "bordered textures are not covered";
            return false;
        }
        layout.format = fetch.Format();
        const Format* format = FindFormat(layout.format);
        if (format == nullptr)
        {
            reason = "the format is not covered";
            return false;
        }
        layout.endian = fetch.Endian();
        layout.tiled = fetch.Tiled();
        uint32_t size = fetch.d[2];
        if (layout.dimension == Dimension::k1D)
        {
            layout.width = (size & 0xFFFFFF) + 1;
            layout.height = 1;
            if (layout.tiled || fetch.PackedMips() || layout.width > kMaxSize1D)
            {
                reason = "the 1D texture layout is not covered";
                return false;
            }
        }
        else
        {
            layout.width = fetch.Width();
            layout.height = fetch.Height();
            if (layout.width > kMaxSize2D || layout.height > kMaxSize2D ||
                (layout.dimension == Dimension::kCube && layout.width != layout.height))
            {
                reason = "the texture size is not covered";
                return false;
            }
            if (fetch.PitchDiv32() == 0)
            {
                reason = "a texture without a pitch is not covered";
                return false;
            }
        }
        layout.faces = layout.dimension == Dimension::kCube ? 6 : 1;

        // The levels in memory: the mips need a mip address, level 0 a base address.
        uint32_t baseAddress = fetch.BaseAddress();
        uint32_t mipAddress = fetch.MipAddress();
        uint32_t sizeMaxLevel = Log2Floor(std::max(layout.width, layout.height));
        if (mipAddress != 0)
        {
            layout.mipMin = std::min(fetch.MipMin(), sizeMaxLevel);
            layout.mipMax = std::max(std::min(fetch.MipMax(), sizeMaxLevel), layout.mipMin);
        }
        layout.hasBase = baseAddress != 0;
        if (layout.mipMax != 0)
        {
            if (!layout.hasBase)
                layout.mipMin = std::max(layout.mipMin, 1u);
            if (layout.mipMin != 0)
                layout.hasBase = false;
        }
        if (!layout.hasBase && layout.mipMax == 0)
        {
            reason = "a texture without memory is not covered";
            return false;
        }

        uint32_t log2Bytes = Log2Ceil(format->bytesPerBlock);
        uint32_t packedLevel = fetch.PackedMips() ? PackedMipLevel(layout.width, layout.height) : kNone;
        auto blocks = [&](uint32_t texels, uint32_t level) {
            return (std::max(texels >> level, 1u) + format->block - 1) / format->block;
        };
        // The bytes one face of a stored place reaches: its own level, or every level of a packed
        // tail stored in it.
        auto extent = [&](const Stored& stored, uint32_t first, uint32_t last) {
            uint32_t right = 0;
            uint32_t bottom = 0;
            for (uint32_t level = first; level <= last; level++)
            {
                uint32_t x = 0;
                uint32_t y = 0;
                if (level >= packedLevel)
                    PackedMipOffset(layout.width, layout.height, level, x, y);
                right = std::max(right, x / format->block + blocks(layout.width, level));
                bottom = std::max(bottom, y / format->block + blocks(layout.height, level));
            }
            return layout.tiled ? TiledExtent(right, bottom, stored.pitchBlocks, log2Bytes)
                                : stored.rowPitch * (bottom - 1) + format->bytesPerBlock * right;
        };

        // The stored mips follow each other from the mip address; a packed tail is stored once at
        // the first packed level (or as mip 0 when the whole chain is packed).
        uint32_t lastStored = std::min(layout.mipMax, packedLevel);
        std::vector<uint32_t> mipOffsets(lastStored + 1, 0);
        uint32_t offset = 0;
        for (uint32_t stored = packedLevel == 0 ? 0 : 1; stored <= lastStored && layout.mipMax != 0; stored++)
        {
            mipOffsets[stored] = offset;
            offset += StoredPitches(fetch, *format, layout, false, stored).faceStride * layout.faces;
        }

        layout.levels.resize(layout.mipMax + 1);
        for (uint32_t level = 0; level <= layout.mipMax; level++)
        {
            if (level == 0 && !layout.hasBase)
                continue;
            LevelPlace& place = layout.levels[level];
            bool packed = level >= packedLevel;
            Stored stored;
            if (level == 0)
            {
                stored = StoredPitches(fetch, *format, layout, true, 0);
                place.address = baseAddress;
                place.extent = extent(stored, 0, 0);
            }
            else
            {
                uint32_t storedLevel = packed ? packedLevel : level;
                stored = StoredPitches(fetch, *format, layout, false, storedLevel);
                place.address = mipAddress + mipOffsets[storedLevel];
                place.extent = packed ? extent(stored, std::max(packedLevel, 1u), layout.mipMax) : extent(stored, level, level);
            }
            place.rowPitch = stored.rowPitch;
            place.faceStride = stored.faceStride;
            if (packed)
            {
                PackedMipOffset(layout.width, layout.height, level, place.x0, place.y0);
                place.x0 /= format->block;
                place.y0 /= format->block;
            }
            uint64_t end = uint64_t(place.address) + uint64_t(layout.faces - 1) * place.faceStride + place.extent;
            if (place.address >= kGpuMemory || end > kGpuMemory || end - place.address > kMaxBytes)
            {
                reason = "the texture memory is out of range";
                return false;
            }
        }
        return true;
    }

    std::vector<std::pair<uint32_t, uint32_t>> MemoryRanges(const Layout& layout)
    {
        std::vector<std::pair<uint32_t, uint32_t>> ranges;
        for (uint32_t level = layout.hasBase ? 0 : 1; level < layout.levels.size(); level++)
        {
            const LevelPlace& place = layout.levels[level];
            ranges.emplace_back(place.address, (layout.faces - 1) * place.faceStride + place.extent);
        }
        std::sort(ranges.begin(), ranges.end());
        std::vector<std::pair<uint32_t, uint32_t>> merged;
        for (const auto& [address, bytes] : ranges)
        {
            if (!merged.empty() && address <= merged.back().first + merged.back().second)
            {
                uint32_t end = std::max(merged.back().first + merged.back().second, address + bytes);
                merged.back().second = end - merged.back().first;
            }
            else
            {
                merged.emplace_back(address, bytes);
            }
        }
        return merged;
    }
}
