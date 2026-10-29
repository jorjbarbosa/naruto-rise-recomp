#pragma once

#include <string>
#include <vector>

// The graphics cards that can run Direct3D 12, by the name Windows gives them:
// the values of the game's graphics_device option. Listed once, on first use.
const std::vector<std::string>& GraphicsDevices(bool vulkan = false);
