#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace settings
{
    enum class ImageFit : uint8_t;
}

namespace gpu
{
    // The graphics API the renderer drives through Plume.
    enum class GpuApi
    {
        D3D12,
        Vulkan,
    };

    // How the present fits the picture in the window.
    enum class ImageFit
    {
        Aspect,  // the largest rectangle of the picture's shape, centered, with black bars
        Stretch, // the whole window
        Integer, // the largest whole multiple of the picture's size, centered; as Aspect when none fits
    };

    // Renderer settings: the renderer's input, so that renderer/ does not depend on the settings
    // store. vsync, adapter and the image quality ones come from the store (settings/settings.h,
    // where their variables override the file; the settings page changes them while running through
    // the Renderer's setters); the API and the debug layer from the environment alone, as developer
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
        // video.resolution_scale: the internal resolution, as a factor of the title's 1280x720, 1 to 4
        // (fractions allowed: 1.5 is 1920x1080); kAutoResolutionScale follows the size the picture is
        // shown at in the window, from 1 to 4. Lowered to what the device's textures allow.
        float resolutionScale = 1.0f;
        // video.msaa: the sample count of every surface the title makes multisampled (its own are 2
        // and 4; single-sampled ones stay so), 1, 2, 4 or 8; 0 keeps the title's. Lowered to what the
        // device supports.
        uint32_t msaa = 0;
        // video.anisotropy: the anisotropic filter of every trilinear fetch of the title's own textures
        // (1: none), 1, 2, 4, 8 or 16, in place of the title's choice, which mostly asks for none; 0
        // keeps the title's.
        uint32_t anisotropy = 0;
        // video.transparency_aa: the alpha-tested draws of opaque color into multisampled targets
        // (cut-out leaves, grass, hair) spread their cut-out edge over the samples by alpha to coverage
        // instead of the test (Draws::Draw).
        bool transparencyAa = false;
        ImageFit imageFit = ImageFit::Aspect; // video.image_fit
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

        // The store's values (its defaults before it is loaded); the API and the debug layer from the
        // environment.
        static Settings FromStore();
    };

    // The store's image fit as the renderer's.
    ImageFit ToImageFit(::settings::ImageFit fit);
}
