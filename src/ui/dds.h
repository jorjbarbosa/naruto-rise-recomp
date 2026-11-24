#pragma once

// DDS files of the game's menus (Naruto_clip.SRA): DXT1, DXT3 and DXT5 with their mip levels.

#include "gpu/ui.h"

#include <cstdint>
#include <span>
#include <string>

namespace ui
{
    // The texture of a DDS file, its levels in the layout gpu::ui wants. False, with the reason in
    // error, for anything else (other formats, a size that is not a multiple of 4, a short file).
    bool ParseDds(std::span<const uint8_t> file, gpu::ui::TextureDesc& out, std::string& error);
}
