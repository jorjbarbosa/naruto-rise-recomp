#pragma once

#include <cstdlib>
#include <cstring>
#include <string>

namespace gpu
{
    // Renderer settings. Defaults, with a few environment overrides.
    struct Settings
    {
        bool debugLayer = false; // ROAN_GPU_DEBUG=1: D3D12 debug layer
        bool vsync = true;       // ROAN_VSYNC=0: present without waiting for the vertical blank
        std::string adapter;     // ROAN_ADAPTER=<name>: adapter as listed in the log (default: Plume's choice)

        static Settings FromEnvironment()
        {
            Settings settings;
            if (const char* value = std::getenv("ROAN_GPU_DEBUG"))
                settings.debugLayer = std::strcmp(value, "1") == 0;
            if (const char* value = std::getenv("ROAN_VSYNC"))
                settings.vsync = std::strcmp(value, "0") != 0;
            if (const char* value = std::getenv("ROAN_ADAPTER"))
                settings.adapter = value;
            return settings;
        }
    };
}
