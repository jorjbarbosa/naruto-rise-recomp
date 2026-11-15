// Guest texture formats: the host format table, detiling, endian swaps and texel conversions.

#include "gpu/textures/formats.h"

#include <bit>
#include <cstring>
#include <utility>

namespace gpu::textures
{
    namespace
    {
        using HF = plume::RenderFormat;
        using C = Conversion;

        // The formats the game uses: those of its assets (k_8, k_DXT1, k_DXT4_5, k_DXN, k_CTX1) and
        // those it makes at run time (k_8_8_8_8 and the depth formats of the resolves, k_5_6_5 of the
        // save thumbnail, the float formats of the vertex textures). The host swizzle replicates the
        // last host component into the missing ones (RRRR, RGGG), and k_5_6_5 reads its fourth
        // component from blue.
        constexpr Format kFormats[] = {
            { 2, 1, 1, HF::R8_UNORM, 1, { 0, 0, 0, 0 }, C::None },                      // k_8
            { 4, 1, 2, HF::R8G8B8A8_UNORM, 4, { 0, 1, 2, 2 }, C::R5G6B5ToRGBA8 },       // k_5_6_5
            { 6, 1, 4, HF::R8G8B8A8_UNORM, 4, { 0, 1, 2, 3 }, C::None },                // k_8_8_8_8
            { 18, 4, 8, HF::BC1_UNORM, 8, { 0, 1, 2, 3 }, C::None },                    // k_DXT1
            { 20, 4, 16, HF::BC3_UNORM, 16, { 0, 1, 2, 3 }, C::None },                  // k_DXT4_5
            { 22, 1, 4, HF::R32_FLOAT, 4, { 0, 0, 0, 0 }, C::DepthUnorm },              // k_24_8
            { 23, 1, 4, HF::R32_FLOAT, 4, { 0, 0, 0, 0 }, C::DepthFloat },              // k_24_8_FLOAT
            { 32, 1, 8, HF::R16G16B16A16_FLOAT, 8, { 0, 1, 2, 3 }, C::None },           // k_16_16_16_16_FLOAT
            { 36, 1, 4, HF::R32_FLOAT, 4, { 0, 0, 0, 0 }, C::None },                    // k_32_FLOAT
            { 37, 1, 8, HF::R32G32_FLOAT, 8, { 0, 1, 1, 1 }, C::None },                 // k_32_32_FLOAT
            { 38, 1, 16, HF::R32G32B32A32_FLOAT, 16, { 0, 1, 2, 3 }, C::None },         // k_32_32_32_32_FLOAT
            { 49, 4, 16, HF::BC5_UNORM, 16, { 0, 1, 1, 1 }, C::None },                  // k_DXN
            { 60, 4, 8, HF::R8G8_UNORM, 2, { 0, 1, 1, 1 }, C::CTX1ToRG8 },              // k_CTX1
        };

        // The block-compressed formats decompressed into texels.
        constexpr Format kDecompressedFormats[] = {
            { 18, 4, 8, HF::R8G8B8A8_UNORM, 4, { 0, 1, 2, 3 }, C::DXT1ToRGBA8 },
            { 20, 4, 16, HF::R8G8B8A8_UNORM, 4, { 0, 1, 2, 3 }, C::DXT5ToRGBA8 },
            { 49, 4, 16, HF::R8G8_UNORM, 2, { 0, 1, 1, 1 }, C::DXNToRG8 },
        };

        uint16_t Read16(const uint8_t* p)
        {
            uint16_t value;
            memcpy(&value, p, 2);
            return value;
        }

        uint32_t Read32(const uint8_t* p)
        {
            uint32_t value;
            memcpy(&value, p, 4);
            return value;
        }

        // The DXT decoders: 565 endpoints widened by bit replication, interpolated colors and
        // alphas with truncating divisions.
        void Endpoint565(uint32_t color, uint32_t rgb[3])
        {
            uint32_t r = (color >> 11) & 31;
            uint32_t g = (color >> 5) & 63;
            uint32_t b = color & 31;
            rgb[0] = (r << 3) | (r >> 2);
            rgb[1] = (g << 2) | (g >> 4);
            rgb[2] = (b << 3) | (b >> 2);
        }

        // The 16 RGBA8 texels of a color block; dxt1 allows the three-color mode (c0 <= c1).
        void ColorBlock(const uint8_t* block, bool dxt1, uint8_t* texels)
        {
            uint32_t c0 = Read16(block);
            uint32_t c1 = Read16(block + 2);
            uint32_t indices = Read32(block + 4);
            uint32_t e0[3];
            uint32_t e1[3];
            Endpoint565(c0, e0);
            Endpoint565(c1, e1);
            bool three = dxt1 && c0 <= c1;
            uint32_t palette[4][4];
            for (uint32_t k = 0; k < 3; k++)
            {
                palette[0][k] = e0[k];
                palette[1][k] = e1[k];
                palette[2][k] = three ? (e0[k] + e1[k]) / 2 : (2 * e0[k] + e1[k]) / 3;
                palette[3][k] = three ? 0 : (e0[k] + 2 * e1[k]) / 3;
            }
            palette[0][3] = palette[1][3] = palette[2][3] = 255;
            palette[3][3] = three ? 0 : 255;
            for (uint32_t i = 0; i < 16; i++)
                for (uint32_t k = 0; k < 4; k++)
                    texels[4 * i + k] = uint8_t(palette[(indices >> (2 * i)) & 3][k]);
        }

        // The 16 values of an alpha block (DXT5 alpha, each DXN channel).
        void AlphaBlock(const uint8_t* block, uint8_t values[16])
        {
            uint32_t a0 = block[0];
            uint32_t a1 = block[1];
            uint64_t indices = 0;
            for (uint32_t i = 0; i < 6; i++)
                indices |= uint64_t(block[2 + i]) << (8 * i);
            uint32_t palette[8] = { a0, a1 };
            if (a0 > a1)
            {
                for (uint32_t i = 2; i < 8; i++)
                    palette[i] = ((8 - i) * a0 + (i - 1) * a1) / 7;
            }
            else
            {
                for (uint32_t i = 2; i < 6; i++)
                    palette[i] = ((6 - i) * a0 + (i - 1) * a1) / 5;
                palette[6] = 0;
                palette[7] = 255;
            }
            for (uint32_t i = 0; i < 16; i++)
                values[i] = uint8_t(palette[(indices >> (3 * i)) & 7]);
        }

        // One 4x4 block into 16 host texels.
        void DecompressBlock(Conversion conversion, const uint8_t* block, uint8_t* texels)
        {
            switch (conversion)
            {
            case C::DXT1ToRGBA8:
                ColorBlock(block, true, texels);
                break;
            case C::DXT5ToRGBA8:
            {
                ColorBlock(block + 8, false, texels);
                uint8_t alpha[16];
                AlphaBlock(block, alpha);
                for (uint32_t i = 0; i < 16; i++)
                    texels[4 * i + 3] = alpha[i];
                break;
            }
            case C::DXNToRG8:
            {
                uint8_t r[16];
                uint8_t g[16];
                AlphaBlock(block, r);
                AlphaBlock(block + 8, g);
                for (uint32_t i = 0; i < 16; i++)
                {
                    texels[2 * i] = r[i];
                    texels[2 * i + 1] = g[i];
                }
                break;
            }
            case C::CTX1ToRG8:
            {
                // g0, r0, g1, r1, then 2-bit indices.
                uint32_t g0 = block[0];
                uint32_t r0 = block[1];
                uint32_t g1 = block[2];
                uint32_t r1 = block[3];
                uint32_t indices = Read32(block + 4);
                uint32_t r[4] = { r0, r1, (2 * r0 + r1) / 3, (r0 + 2 * r1) / 3 };
                uint32_t g[4] = { g0, g1, (2 * g0 + g1) / 3, (g0 + 2 * g1) / 3 };
                for (uint32_t i = 0; i < 16; i++)
                {
                    uint32_t k = (indices >> (2 * i)) & 3;
                    texels[2 * i] = uint8_t(r[k]);
                    texels[2 * i + 1] = uint8_t(g[k]);
                }
                break;
            }
            default:
                break;
            }
        }

        float DepthUnormToFloat(uint32_t value)
        {
            uint32_t depth = value >> 8;
            return float(depth + (depth >> 23)) * 0x1p-24f;
        }

        // The 20e4 float in the 24 high bits: 4-bit exponent, 20-bit mantissa, bias 15.
        uint32_t DepthFloatToFloat(uint32_t value)
        {
            uint32_t depth = value >> 8;
            if (depth == 0)
                return 0;
            uint32_t mantissa = depth & 0xFFFFF;
            uint32_t exponent = depth >> 20;
            if (exponent == 0)
            {
                // A denormal is normalized: its top bit moves to bit 20.
                uint32_t shift = 0;
                while ((mantissa & 0x100000) == 0)
                {
                    mantissa <<= 1;
                    shift++;
                }
                mantissa &= 0xFFFFF;
                exponent = 1 - shift;
            }
            return ((exponent << 23) + 0x38000000u) + (mantissa << 3);
        }

        // The swap mode of a level's blocks. The fetch swaps by element size: 8-bit texels never,
        // 16-bit ones in 8in16 only, larger blocks every 32-bit word in each mode.
        uint32_t SwapMode(const GuestLevel& level)
        {
            uint32_t bytes = level.bytesPerBlock;
            return bytes == 1 ? 0 : bytes == 2 ? (level.endian == 1 ? 1 : 0) : level.endian;
        }

        // Swaps one block in place; each mode is its own inverse.
        void SwapBlock(uint8_t* block, uint32_t bytes, uint32_t mode)
        {
            if (mode == 1)
            {
                for (uint32_t i = 0; i < bytes; i += 2)
                    std::swap(block[i], block[i + 1]);
            }
            else if (mode == 2)
            {
                for (uint32_t i = 0; i < bytes; i += 4)
                {
                    std::swap(block[i], block[i + 3]);
                    std::swap(block[i + 1], block[i + 2]);
                }
            }
            else if (mode == 3)
            {
                for (uint32_t i = 0; i < bytes; i += 4)
                {
                    std::swap(block[i], block[i + 2]);
                    std::swap(block[i + 1], block[i + 3]);
                }
            }
        }

        // The byte offset of block (x, y) of a level in its place.
        uint32_t BlockOffset(const GuestLevel& level, uint32_t x, uint32_t y)
        {
            uint32_t bytes = level.bytesPerBlock;
            uint32_t log2Bytes = uint32_t(std::bit_width(bytes)) - 1; // bytes is a power of two
            uint32_t column = level.x0 + x;
            uint32_t row = level.y0 + y;
            return level.tiled ? TiledOffset2D(column, row, level.rowPitch / bytes, log2Bytes)
                               : row * level.rowPitch + column * bytes;
        }
    }

    const Format* FindFormat(uint32_t format, bool decompressed)
    {
        if (decompressed)
        {
            for (const Format& entry : kDecompressedFormats)
                if (entry.guest == format)
                    return &entry;
            return nullptr;
        }
        for (const Format& entry : kFormats)
            if (entry.guest == format)
                return &entry;
        return nullptr;
    }

    uint32_t TiledOffset2D(uint32_t x, uint32_t y, uint32_t pitchBlocks, uint32_t log2Bytes)
    {
        // The row part: macro tile row, micro row bits.
        uint32_t rowMacro = ((y / 32) * (pitchBlocks / 32)) << (log2Bytes + 7);
        uint32_t rowMicro = ((y & 6) << 2) << log2Bytes;
        uint32_t row = rowMacro + ((rowMicro & ~0xFu) << 1) + (rowMicro & 0xF) + ((y & 8) << (3 + log2Bytes)) +
            ((y & 1) << 4);
        // The column part, then the bank and pipe interleave.
        uint32_t columnMacro = (x / 32) << (log2Bytes + 7);
        uint32_t columnMicro = (x & 7) << log2Bytes;
        uint32_t offset = row + columnMacro + ((columnMicro & ~0xFu) << 1) + (columnMicro & 0xF);
        uint32_t address = ((offset & ~0x1FFu) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) + ((y & 16) << 7) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
        return (address >> log2Bytes) << log2Bytes;
    }

    bool ReadLevel(const GuestLevel& level, std::vector<uint8_t>& out)
    {
        uint32_t bytes = level.bytesPerBlock;
        uint32_t mode = SwapMode(level);
        out.resize(size_t(level.widthBlocks) * level.heightBlocks * bytes);
        uint8_t* to = out.data();
        for (uint32_t y = 0; y < level.heightBlocks; y++)
        {
            for (uint32_t x = 0; x < level.widthBlocks; x++, to += bytes)
            {
                uint32_t at = BlockOffset(level, x, y);
                if (uint64_t(at) + bytes > level.bytes)
                    return false;
                memcpy(to, level.memory + at, bytes);
                SwapBlock(to, bytes, mode);
            }
        }
        return true;
    }

    bool WriteLevel(const GuestLevel& level, uint8_t* memory, const std::vector<uint8_t>& blocks)
    {
        uint32_t bytes = level.bytesPerBlock;
        uint32_t mode = SwapMode(level);
        if (blocks.size() < size_t(level.widthBlocks) * level.heightBlocks * bytes)
            return false;
        // Every block is checked first, so a failure stores nothing.
        for (uint32_t y = 0; y < level.heightBlocks; y++)
            for (uint32_t x = 0; x < level.widthBlocks; x++)
                if (uint64_t(BlockOffset(level, x, y)) + bytes > level.bytes)
                    return false;
        const uint8_t* from = blocks.data();
        uint8_t block[16];
        for (uint32_t y = 0; y < level.heightBlocks; y++)
        {
            for (uint32_t x = 0; x < level.widthBlocks; x++, from += bytes)
            {
                uint32_t at = BlockOffset(level, x, y);
                memcpy(block, from, bytes);
                SwapBlock(block, bytes, mode);
                memcpy(memory + at, block, bytes);
            }
        }
        return true;
    }

    bool PackColors(uint32_t format, uint32_t swizzle, const std::vector<uint8_t>& rgba, std::vector<uint8_t>& blocks)
    {
        // Bits of each guest component (X, Y, Z, W), from bit 0 of the block.
        static constexpr uint8_t k8888Bits[4] = { 8, 8, 8, 8 };
        static constexpr uint8_t k565Bits[4] = { 5, 6, 5, 0 };
        const uint8_t* bits = format == k8_8_8_8 ? k8888Bits : format == k5_6_5 ? k565Bits : nullptr;
        if (bits == nullptr)
            return false;
        uint32_t bytes = format == k8_8_8_8 ? 4 : 2;
        size_t texels = rgba.size() / 4;
        blocks.resize(texels * bytes);
        for (size_t i = 0; i < texels; i++)
        {
            // A component no color is read from keeps all its bits set (an alpha the swizzle drops);
            // one that several colors read (a luminance swizzle) takes the first of them.
            uint32_t value = 0;
            uint32_t shift = 0;
            uint32_t component[4] = { ~0u, ~0u, ~0u, ~0u };
            bool taken[4] = {};
            for (uint32_t color = 0; color < 4; color++)
            {
                uint32_t select = (swizzle >> (3 * color)) & 7;
                if (select < 4 && bits[select] != 0 && !taken[select])
                {
                    taken[select] = true;
                    uint32_t max = (1u << bits[select]) - 1;
                    component[select] = (rgba[4 * i + color] * max + 127) / 255;
                }
            }
            for (uint32_t c = 0; c < 4; c++)
            {
                value |= (component[c] & ((1u << bits[c]) - 1)) << shift;
                shift += bits[c];
            }
            memcpy(&blocks[i * bytes], &value, bytes);
        }
        return true;
    }

    void ConvertLevel(const Format& format, std::vector<uint8_t>& data, uint32_t width, uint32_t height)
    {
        switch (format.conversion)
        {
        case C::None:
            return;
        case C::R5G6B5ToRGBA8:
        {
            std::vector<uint8_t> texels(data.size() * 2);
            for (size_t i = 0; i < data.size() / 2; i++)
            {
                uint32_t value = Read16(&data[2 * i]);
                uint32_t r = value & 31;
                uint32_t g = (value >> 5) & 63;
                uint32_t b = value >> 11;
                texels[4 * i] = uint8_t((r << 3) | (r >> 2));
                texels[4 * i + 1] = uint8_t((g << 2) | (g >> 4));
                texels[4 * i + 2] = uint8_t((b << 3) | (b >> 2));
                texels[4 * i + 3] = 255;
            }
            data.swap(texels);
            return;
        }
        case C::DepthUnorm:
            for (size_t i = 0; i + 4 <= data.size(); i += 4)
            {
                float depth = DepthUnormToFloat(Read32(&data[i]));
                memcpy(&data[i], &depth, 4);
            }
            return;
        case C::DepthFloat:
            for (size_t i = 0; i + 4 <= data.size(); i += 4)
            {
                uint32_t depth = DepthFloatToFloat(Read32(&data[i]));
                memcpy(&data[i], &depth, 4);
            }
            return;
        default:
            break;
        }

        // Decompression: the blocks cover the level rounded up to 4 texels, cropped here.
        uint32_t widthBlocks = (width + 3) / 4;
        uint32_t heightBlocks = (height + 3) / 4;
        uint32_t texelBytes = format.hostBytes;
        std::vector<uint8_t> texels(size_t(width) * height * texelBytes);
        uint8_t blockTexels[16 * 4];
        for (uint32_t by = 0; by < heightBlocks; by++)
        {
            for (uint32_t bx = 0; bx < widthBlocks; bx++)
            {
                DecompressBlock(format.conversion, data.data() + (size_t(by) * widthBlocks + bx) * format.bytesPerBlock,
                    blockTexels);
                for (uint32_t y = 0; y < 4 && by * 4 + y < height; y++)
                    for (uint32_t x = 0; x < 4 && bx * 4 + x < width; x++)
                        memcpy(&texels[(size_t(by * 4 + y) * width + bx * 4 + x) * texelBytes],
                            &blockTexels[(y * 4 + x) * texelBytes], texelBytes);
            }
        }
        data.swap(texels);
    }
}
