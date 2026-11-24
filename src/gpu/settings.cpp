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
        if (const char* value = std::getenv("ROAN_ULTRAWIDE"))
            settings.ultrawide = std::strcmp(value, "0") != 0;
        if (const char* value = std::getenv("ROAN_RESOLUTION_SCALE"))
        {
            char* end = nullptr;
            float scale = std::strtof(value, &end);
            if (std::strcmp(value, "auto") == 0)
                settings.resolutionScale = kAutoResolutionScale;
            else if (end != value && *end == '\0' && scale >= kMinResolutionScale && scale <= kMaxResolutionScale)
                settings.resolutionScale = scale;
            else
                Log("gpu: ROAN_RESOLUTION_SCALE=%s is not auto or a number from %g to %g; the scale stays 1", value,
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
        if (const char* value = std::getenv("ROAN_TRANSPARENCY_AA"))
            settings.transparencyAa = std::strcmp(value, "1") == 0;
        return settings;
    }
}
