// Renderer settings (settings.h): the settings store's and the environment's.

#include "gpu/settings.h"

#include "log.h"
#include "settings/settings.h"

#include <cstdlib>
#include <cstring>

namespace gpu
{
    Settings Settings::FromStore()
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
        settings.vsync = ::settings::VSync();
        settings.adapter = ::settings::Adapter();
        settings.resolutionScale = ::settings::ResolutionScale();
        settings.msaa = ::settings::Multisampling();
        settings.anisotropy = ::settings::Anisotropy();
        settings.transparencyAa = ::settings::TransparencyAa();
        settings.imageFit = ToImageFit(::settings::GetImageFit());
        return settings;
    }

    ImageFit ToImageFit(::settings::ImageFit fit)
    {
        switch (fit)
        {
        case ::settings::ImageFit::Aspect: return ImageFit::Aspect;
        case ::settings::ImageFit::Stretch: return ImageFit::Stretch;
        case ::settings::ImageFit::Integer: return ImageFit::Integer;
        }
        return ImageFit::Aspect;
    }
}
