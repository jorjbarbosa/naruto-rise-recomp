#pragma once

#include "log.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace gpu
{
    // The graphics API the renderer drives through Plume.
    enum class GpuApi
    {
        D3D12,
        Vulkan,
    };

    // Renderer settings. Defaults, with a few environment overrides.
    struct Settings
    {
        // ROAN_GPU_API=d3d12|vulkan|none. d3d12 or vulkan picks the API; none turns the renderer off
        // (renderer below). Any other value, and d3d12 where it is not built (ROAN_D3D12, Windows only;
        // logged), keeps the default: D3D12 where it is built, else Vulkan.
#ifdef ROAN_D3D12
        GpuApi api = GpuApi::D3D12;
#else
        GpuApi api = GpuApi::Vulkan;
#endif
        // False for ROAN_GPU_API=none: no renderer is made. The game runs and presents nothing, its
        // frames paced by the limiter; a replay refuses to run. Read by the callers of
        // Renderer::Create, not by the renderer.
        bool renderer = true;
        bool debugLayer = false; // ROAN_GPU_DEBUG=1: the API's debug layer
        bool syncValidation = false; // ROAN_GPU_DEBUG=sync: the debug layer, plus Vulkan's synchronization validation
        bool vsync = true;       // ROAN_VSYNC=0: present without waiting for the vertical blank
        std::string adapter;     // ROAN_ADAPTER=<name>: adapter as listed in the log (default: Plume's choice)
        std::filesystem::path pipelineList; // the pipeline list file (Pipelines); empty: none
        std::filesystem::path pipelineCache; // the driver's pipeline cache file, on Vulkan (Pipelines); empty: none
        bool recordFrames = false; // frames may be recorded (Renderer::RecordNextFrame); set by the caller, from gpu/guest/compare.h

        // The pipeline files in the folder the renderer keeps its caches in.
        void SetCacheDirectory(const std::filesystem::path& directory)
        {
            pipelineList = directory / "pipelines.bin";
            pipelineCache = directory / "vulkan-pipeline-cache.bin";
        }

        static Settings FromEnvironment()
        {
            Settings settings;
            if (const char* value = std::getenv("ROAN_GPU_API"))
            {
                if (std::strcmp(value, "d3d12") == 0)
                {
#ifdef ROAN_D3D12
                    settings.api = GpuApi::D3D12;
#else
                    Log("gpu: ROAN_GPU_API=d3d12, but this build has no D3D12; the renderer uses Vulkan");
#endif
                }
                else if (std::strcmp(value, "vulkan") == 0)
                {
                    settings.api = GpuApi::Vulkan;
                }
                else if (std::strcmp(value, "none") == 0)
                {
                    settings.renderer = false;
                }
            }
            if (const char* value = std::getenv("ROAN_GPU_DEBUG"))
            {
                settings.syncValidation = std::strcmp(value, "sync") == 0;
                settings.debugLayer = std::strcmp(value, "1") == 0 || settings.syncValidation;
            }
            if (const char* value = std::getenv("ROAN_VSYNC"))
                settings.vsync = std::strcmp(value, "0") != 0;
            if (const char* value = std::getenv("ROAN_ADAPTER"))
                settings.adapter = value;
            return settings;
        }
    };
}
