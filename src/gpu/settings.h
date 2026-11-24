#pragma once

#include <cstdint>
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

    // Renderer settings: the renderer's input, so that renderer/ does not depend on the settings
    // store. vsync and adapter come from the store (settings/settings.h, where their variables
    // override the file); the others from the environment alone, as developer and image quality
    // switches (FromStore, settings.cpp).
    struct Settings
    {
        static constexpr float kMinResolutionScale = 1.0f;
        static constexpr float kMaxResolutionScale = 4.0f;
        static constexpr float kAutoResolutionScale = 0.0f;

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
        bool vsync = true;       // video.vsync: wait for the vertical blank when presenting
        // ROAN_ULTRAWIDE=0: the picture stays 16:9, with bars in a window of another shape. Otherwise
        // a window wider than 16:9 is filled (gpu::WideStretch).
        bool ultrawide = true;
        // ROAN_RESOLUTION_SCALE=<1 to 4>|auto: the internal resolution, as a factor of the title's
        // 1280x720 (fractions allowed: 1.5 is 1920x1080); auto (kAutoResolutionScale) follows the size
        // the picture is shown at in the window, from 1 to 4. Anything else: logged, kept at 1.
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
        // ROAN_TRANSPARENCY_AA=1: the alpha-tested draws of opaque color into multisampled targets
        // (cut-out leaves, grass, hair) spread their cut-out edge over the samples by alpha to coverage
        // instead of the test (Draws::Draw). Off by default.
        bool transparencyAa = false;
        std::string adapter; // video.adapter: the adapter as listed in the log (empty: Plume's choice)
        std::filesystem::path pipelineList; // the pipeline list file (Pipelines); empty: none
        std::filesystem::path pipelineCache; // the driver's pipeline cache file, on Vulkan (Pipelines); empty: none
        bool recordFrames = false; // frames may be recorded (Renderer::RecordNextFrame); set by the caller, from gpu/guest/compare.h

        // The pipeline files in the folder the renderer keeps its caches in.
        void SetCacheDirectory(const std::filesystem::path& directory)
        {
            pipelineList = directory / "pipelines.bin";
            pipelineCache = directory / "vulkan-pipeline-cache.bin";
        }

        // vsync and adapter from the settings store (its defaults before it is loaded), the others
        // from the environment. Values that are not valid are logged and left at their defaults.
        static Settings FromStore();
    };
}
