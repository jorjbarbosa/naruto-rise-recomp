// DDS to gpu::ui texture (dds.h).

#include "ui/dds.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace ui
{
    namespace
    {
        // Offsets in the file: the 4-byte magic, then DDS_HEADER (124 bytes) with its DDS_PIXELFORMAT.
        constexpr size_t kHeaderSize = 128;
        constexpr size_t kHeight = 12;
        constexpr size_t kWidth = 16;
        constexpr size_t kMipCount = 28;
        constexpr size_t kPixelFlags = 80;
        constexpr size_t kFourCc = 84;
        constexpr uint32_t kPixelFourCc = 0x4; // DDPF_FOURCC

        uint32_t Read32(std::span<const uint8_t> file, size_t offset)
        {
            uint32_t value;
            std::memcpy(&value, file.data() + offset, 4);
            return value;
        }

        constexpr uint32_t FourCc(const char (&text)[5])
        {
            return uint32_t(uint8_t(text[0])) | uint32_t(uint8_t(text[1])) << 8 | uint32_t(uint8_t(text[2])) << 16 |
                   uint32_t(uint8_t(text[3])) << 24;
        }
    }

    bool ParseDds(std::span<const uint8_t> file, gpu::ui::TextureDesc& out, std::string& error)
    {
        if (file.size() < kHeaderSize || std::memcmp(file.data(), "DDS ", 4) != 0 || Read32(file, 4) != 124)
        {
            error = "not a DDS file";
            return false;
        }
        uint32_t fourCc = Read32(file, kFourCc);
        uint32_t bytesPerBlock = 0;
        if ((Read32(file, kPixelFlags) & kPixelFourCc) != 0)
        {
            if (fourCc == FourCc("DXT1"))
            {
                out.format = gpu::ui::Format::Bc1;
                bytesPerBlock = 8;
            }
            else if (fourCc == FourCc("DXT3"))
            {
                out.format = gpu::ui::Format::Bc2;
                bytesPerBlock = 16;
            }
            else if (fourCc == FourCc("DXT5"))
            {
                out.format = gpu::ui::Format::Bc3;
                bytesPerBlock = 16;
            }
        }
        if (bytesPerBlock == 0)
        {
            error = "not DXT1, DXT3 or DXT5";
            return false;
        }
        out.width = Read32(file, kWidth);
        out.height = Read32(file, kHeight);
        if (out.width == 0 || out.height == 0 || out.width % 4 != 0 || out.height % 4 != 0)
        {
            error = "a size of " + std::to_string(out.width) + "x" + std::to_string(out.height) +
                    ", not a multiple of 4";
            return false;
        }
        uint32_t maxLevels = uint32_t(std::bit_width(std::max(out.width, out.height)));
        uint32_t levels = std::clamp(Read32(file, kMipCount), 1u, maxLevels);

        // The levels the file holds whole; a file cut short keeps the ones before the cut.
        size_t size = 0;
        uint32_t complete = 0;
        for (uint32_t level = 0; level < levels; level++)
        {
            size_t columns = (std::max(out.width >> level, 1u) + 3) / 4;
            size_t rows = (std::max(out.height >> level, 1u) + 3) / 4;
            size_t levelSize = columns * rows * bytesPerBlock;
            if (kHeaderSize + size + levelSize > file.size())
                break;
            size += levelSize;
            complete++;
        }
        if (complete == 0)
        {
            error = "no whole level";
            return false;
        }
        out.levels = complete;
        out.data.assign(file.begin() + kHeaderSize, file.begin() + kHeaderSize + size);
        return true;
    }
}
