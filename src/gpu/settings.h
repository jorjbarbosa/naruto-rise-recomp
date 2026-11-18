#pragma once

#include <filesystem>
#include <string>

namespace gpu
{
    // Renderer settings: the renderer's input, so renderer/ does not depend on the settings store.
    struct Settings
    {
        bool debugLayer = false; // ROAN_GPU_DEBUG=1: D3D12 debug layer (a developer switch, not a setting)
        bool vsync = true;       // video.vsync (settings/settings.h)
        std::string adapter;     // video.adapter: adapter as listed in the log (empty: Plume's choice)
        std::filesystem::path pipelineList; // the pipeline list file (Pipelines); empty: none

        // vsync and adapter from the settings store, debugLayer from the environment (settings.cpp).
        static Settings FromStore();
    };
}
