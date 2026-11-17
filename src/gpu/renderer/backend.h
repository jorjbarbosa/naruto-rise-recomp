#pragma once

// The graphics API under the renderer. Everything that depends on which API Plume drives, beyond
// Plume's own interface, lives in backend.cpp: it is the only renderer file that includes a backend
// header of Plume. The functions that take a Plume object tell the API from the object itself.
// CreateInterface and PrepareDevice run on the thread that creates the renderer, the others on the
// owner thread.

#include "gpu/settings.h"

#include <plume_render_interface.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

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

    // Whether the API created the object: Plume hands back an object even when the API refused to
    // create it. False for null.
    bool IsCreated(const plume::RenderBuffer* buffer);
    bool IsCreated(const plume::RenderTexture* texture);
    bool IsCreated(const plume::RenderPipeline* pipeline);
    bool IsCreated(const plume::RenderSwapChain* swapChain);

    // The constant buffers of a draw of the translated shaders.
    struct DrawConstants
    {
        plume::RenderBufferReference vertex; // vertex shader float constants
        plume::RenderBufferReference pixel;  // pixel shader float constants
        plume::RenderBufferReference shared; // SharedConstants
    };

    // The pipeline layout of the draws: the descriptor sets, and where the translated shaders read
    // the draw constants (root constant buffers on D3D12, device addresses in push constants on
    // Vulkan).
    std::unique_ptr<plume::RenderPipelineLayout> CreateDrawLayout(plume::RenderDevice& device,
        const plume::RenderDescriptorSetDesc* sets, uint32_t setCount);

    // Binds the draw constants on a list whose pipeline layout is the draw layout. bound: what the
    // last call on this list set, empty when nothing is known; only what differs is set again.
    void BindDrawConstants(plume::RenderCommandList& list, const DrawConstants& constants,
        const std::optional<DrawConstants>& bound);

    // Dynamic state Plume's interface has no call for, for the draws recorded after it on the list:
    // the blend constant color and the stencil reference.
    void SetBlendConstants(plume::RenderCommandList& list, const std::array<float, 4>& color);
    void SetStencilReference(plume::RenderCommandList& list, uint32_t reference);

    // How a wait for the swap chain to accept another frame ended.
    enum class LatencyWait
    {
        Ready,       // a present left the queue
        TimedOut,
        Unavailable, // the swap chain has no frame latency signal
    };

    // Waits up to timeoutMs for one present to leave the swap chain's queue (shown or replaced). Each
    // Ready answers one present.
    LatencyWait WaitForFrameLatency(plume::RenderSwapChain& swapChain, uint32_t timeoutMs);

    // Presents the swap chain's current image, held for syncInterval vertical blanks (0: at once).
    // allowTearing lets an immediate present tear when the swap chain supports it. False when the
    // API refuses the present.
    bool PresentSwapChain(plume::RenderSwapChain& swapChain, uint32_t syncInterval, bool allowTearing);
}
