#pragma once

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
        GpuApi api = GpuApi::D3D12; // ROAN_GPU_API=d3d12|vulkan; any other value keeps the default
        bool debugLayer = false; // ROAN_GPU_DEBUG=1: the API's debug layer
        bool syncValidation = false; // ROAN_GPU_DEBUG=sync: the debug layer, plus Vulkan's synchronization validation
        bool vsync = true;       // ROAN_VSYNC=0: present without waiting for the vertical blank
        std::string adapter;     // ROAN_ADAPTER=<name>: adapter as listed in the log (default: Plume's choice)
        std::filesystem::path pipelineList; // the pipeline list file (Pipelines); empty: none
        std::filesystem::path pipelineCache; // the driver's pipeline cache file, on Vulkan (Pipelines); empty: none
        bool recordFrames = false; // ROAN_RECORD_FRAMES set: frames may be recorded (Renderer::RecordNextFrame)

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
                    settings.api = GpuApi::D3D12;
                else if (std::strcmp(value, "vulkan") == 0)
                    settings.api = GpuApi::Vulkan;
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
            settings.recordFrames = std::getenv("ROAN_RECORD_FRAMES") != nullptr;
            return settings;
        }
    };
}
