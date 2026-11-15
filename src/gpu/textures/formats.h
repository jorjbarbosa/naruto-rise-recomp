#pragma once

// Guest texture formats: the host format of each covered GPUTEXTUREFORMAT and the conversion of
// guest blocks into host texels. Pure functions, shared by the guest side (block sizes for the
// layout) and the renderer (detiling and conversion on upload).

#include <plume_render_interface_types.h>

#include <cstdint>
#include <vector>

namespace gpu::textures
{
    // GPUTEXTUREFORMAT of the color resolves.
    constexpr uint32_t k5_6_5 = 4;
    constexpr uint32_t k8_8_8_8 = 6;

    // A conversion of the guest blocks after the endian swap.
    enum class Conversion : uint8_t
    {
        None,
        R5G6B5ToRGBA8,  // k_5_6_5: the host has no 16-bit color format
        DepthUnorm,     // k_24_8: the 24-bit depth as a float
        DepthFloat,     // k_24_8_FLOAT: the 20e4 depth as a float
        DXT1ToRGBA8,    // block-compressed textures whose base is not a multiple of 4 texels
        DXT5ToRGBA8,
        DXNToRG8,
        CTX1ToRG8,      // k_CTX1 always: the host has no CTX1
    };

    struct Format
    {
        uint32_t guest = 0;            // GPUTEXTUREFORMAT
        uint8_t block = 1;             // block width and height in texels
        uint8_t bytesPerBlock = 4;     // guest bytes per block
        plume::RenderFormat host = plume::RenderFormat::UNKNOWN;
        uint8_t hostBytes = 4;         // host bytes per block (per texel when the conversion decompresses)
        uint8_t swizzle[4] = {};       // the host component each guest component reads
        Conversion conversion = Conversion::None;

        bool Decompresses() const { return conversion >= Conversion::DXT1ToRGBA8; }
        bool HostCompressed() const { return block > 1 && !Decompresses(); }
    };

    // The format of a guest texture, or null when it is not covered. decompressed: the host format
    // of a block-compressed texture whose base is not a multiple of 4 texels (the host needs it).
    const Format* FindFormat(uint32_t format, bool decompressed = false);

    // The Xenos 2D tiled address: 32x32-block macro tiles. The byte offset of block (x, y) of a
    // surface whose pitch is pitchBlocks (a multiple of 32), for 2^log2Bytes bytes per block.
    uint32_t TiledOffset2D(uint32_t x, uint32_t y, uint32_t pitchBlocks, uint32_t log2Bytes);

    // One level (or face) of guest memory and where the level lies in it.
    struct GuestLevel
    {
        const uint8_t* memory = nullptr; // the start of its place
        uint32_t bytes = 0;              // readable from memory
        bool tiled = false;
        uint32_t rowPitch = 0;           // bytes between rows of blocks (tiled: the pitch in blocks times the block)
        uint32_t endian = 0;             // 0 none, 1 8in16, 2 8in32, 3 16in32
        uint32_t x0 = 0;                 // offset of the level in its place (packed mip tail), in blocks
        uint32_t y0 = 0;
        uint32_t widthBlocks = 0;
        uint32_t heightBlocks = 0;
        uint32_t bytesPerBlock = 4;
    };

    // The level's blocks, row after row, each swapped by the endian mode as the texture fetch reads
    // it. False when a block lies past the readable bytes.
    bool ReadLevel(const GuestLevel& level, std::vector<uint8_t>& out);

    // The guest blocks of a level of width x height texels as host data: converted in place, or
    // decompressed into texels.
    void ConvertLevel(const Format& format, std::vector<uint8_t>& data, uint32_t width, uint32_t height);

    // The inverse of ReadLevel: stores blocks (row after row, as ReadLevel returns them) into the
    // level's place at memory, tiled and swapped so that the texture fetch reads them back. False, with
    // nothing stored, when a block lies past level.bytes or blocks is short.
    bool WriteLevel(const GuestLevel& level, uint8_t* memory, const std::vector<uint8_t>& blocks);

    // Colors (RGBA8, one texel after another) as the blocks of a color format that the fetch swizzle
    // reads back as those colors: each color goes to the guest component its swizzle selector names
    // (the first color, when several name one). This is what a resolve leaves in memory. Covered:
    // k_8_8_8_8 and k_5_6_5; false for others.
    bool PackColors(uint32_t format, uint32_t swizzle, const std::vector<uint8_t>& rgba, std::vector<uint8_t>& blocks);
}
