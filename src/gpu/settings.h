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
        static constexpr float kMinResolutionScale = 1.0f;
        static constexpr float kMaxResolutionScale = 4.0f;

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
        // ROAN_ULTRAWIDE=0: the picture stays 16:9, with bars in a window of another shape. Otherwise
        // a window wider than 16:9 is filled (gpu::WideStretch).
        bool ultrawide = true;
        // ROAN_RESOLUTION_SCALE=<1 to 4>: the internal resolution, as a factor of the title's 1280x720
        // (fractions allowed: 1.5 is 1920x1080). Out of range or not a number: logged, kept at 1.
        // Lowered to what the device's textures allow (Renderer::Create).
        float resolutionScale = 1.0f;
        // ROAN_MSAA=1|2|4|8: the sample count of every surface the title makes multisampled (its own
        // are 2 and 4; single-sampled ones stay so); 0, the default, keeps the title's. Lowered to
        // what the device supports (Renderer::Create). Any other value: logged, kept at 0.
        uint32_t msaa = 0;
        // ROAN_ANISOTROPY=1|2|4|8|16: the anisotropic filter of every trilinear fetch of the title's
        // own textures (1: none), in place of the title's choice, which mostly asks for none; 0, the
        // default, keeps the title's. Any other value: logged, kept at 0.
        uint32_t anisotropy = 0;
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
            if (const char* value = std::getenv("ROAN_ULTRAWIDE"))
                settings.ultrawide = std::strcmp(value, "0") != 0;
            if (const char* value = std::getenv("ROAN_RESOLUTION_SCALE"))
            {
                char* end = nullptr;
                float scale = std::strtof(value, &end);
                if (end != value && *end == '\0' && scale >= kMinResolutionScale && scale <= kMaxResolutionScale)
                    settings.resolutionScale = scale;
                else
                    Log("gpu: ROAN_RESOLUTION_SCALE=%s is not a number from %g to %g; the scale stays 1", value,
                        double(kMinResolutionScale), double(kMaxResolutionScale));
            }
            if (const char* value = std::getenv("ROAN_MSAA"))
            {
                if (std::strcmp(value, "1") == 0 || std::strcmp(value, "2") == 0 || std::strcmp(value, "4") == 0 ||
                    std::strcmp(value, "8") == 0)
                    settings.msaa = uint32_t(std::atoi(value));
                else
                    Log("gpu: ROAN_MSAA=%s is not 1, 2, 4 or 8; the title's sample counts are kept", value);
            }
            if (const char* value = std::getenv("ROAN_ANISOTROPY"))
            {
                if (std::strcmp(value, "1") == 0 || std::strcmp(value, "2") == 0 || std::strcmp(value, "4") == 0 ||
                    std::strcmp(value, "8") == 0 || std::strcmp(value, "16") == 0)
                    settings.anisotropy = uint32_t(std::atoi(value));
                else
                    Log("gpu: ROAN_ANISOTROPY=%s is not 1, 2, 4, 8 or 16; the title's filters are kept", value);
            }
            if (const char* value = std::getenv("ROAN_ADAPTER"))
                settings.adapter = value;
            return settings;
        }
    };
}
