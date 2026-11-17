#pragma once

// The graphics API under the renderer. Everything that depends on which API Plume drives, beyond
// Plume's own interface, lives in backend.cpp: it is the only renderer file that includes a backend
// header of Plume. Called on the thread that creates the renderer.

#include "gpu/settings.h"

#include <plume_render_interface.h>

#include <memory>

namespace gpu
{
    // The API's name for the log ("D3D12", "Vulkan").
    const char* ApiName(GpuApi api);

    // Plume's interface for settings.api. With settings.debugLayer, the API's debug layer is turned
    // on first when the API has one here. Null when the interface cannot be created (logged).
    std::unique_ptr<plume::RenderInterface> CreateInterface(const Settings& settings);

    // Readies a new device for the renderer and logs what it reports. On Vulkan it checks every
    // feature and limit the translated shaders and the renderer rely on, since Plume enables what the
    // device has but requires none of it. False when the device cannot run the renderer (each missing
    // item logged).
    bool PrepareDevice(plume::RenderDevice& device, const Settings& settings);
}
