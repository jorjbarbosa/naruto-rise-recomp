// Renderer settings (settings.h).

#include "gpu/settings.h"

#include "settings/settings.h"

#include <cstdlib>
#include <cstring>

namespace gpu
{
    Settings Settings::FromStore()
    {
        Settings result;
        if (const char* value = std::getenv("ROAN_GPU_DEBUG"))
            result.debugLayer = std::strcmp(value, "1") == 0;
        result.vsync = ::settings::VSync();
        result.adapter = ::settings::Adapter();
        return result;
    }
}
