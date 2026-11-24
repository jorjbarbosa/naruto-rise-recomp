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
#include <vector>

namespace gpu
{
    // The API's name for the log ("D3D12", "Vulkan").
    const char* ApiName(GpuApi api);

    // Plume's interface for settings.api. With settings.debugLayer, the API's debug layer is turned
    // on first when the API has one here. Null when the interface cannot be created (logged).
    std::unique_ptr<plume::RenderInterface> CreateInterface(const Settings& settings);

    // Readies a new device for the renderer and logs what it reports. On Vulkan it checks every
    // feature, limit and format feature the translated shaders and the renderer rely on, since Plume
    // enables what the device has but requires none of it (sample counts are lowered to what the
    // device has instead: SurfaceSampleCounts). False when the device cannot run the renderer (each missing
    // item logged).
    bool PrepareDevice(plume::RenderDevice& device, const Settings& settings);

    // The sample counts (plume::RenderSampleCount bits) a surface can have on the device: drawn into
    // in both surface formats, and read by the resolve passes.
    uint32_t SurfaceSampleCounts(plume::RenderDevice& device);

    // Whether the API created the object: Plume hands back an object even when the API refused to
    // create it. False for null.
    bool IsCreated(const plume::RenderBuffer* buffer);
    bool IsCreated(const plume::RenderTexture* texture);
    bool IsCreated(const plume::RenderPipeline* pipeline);
    bool IsCreated(const plume::RenderSwapChain* swapChain);

    // The shader format the device takes: DXIL on D3D12, SPIR-V on Vulkan.
    plume::RenderShaderFormat ShaderFormat(plume::RenderDevice& device);

    // The pipeline cache, where the application keeps the driver's compiled pipelines between runs
    // (Vulkan; D3D12 drivers keep their own). Its file holds the cache's data behind a header that
    // ties it to the device, the driver and the translated shaders (shaderHash, ShaderCache::Hash),
    // with a hash of the data.
    bool KeepsPipelineCache(plume::RenderDevice& device);

    // Starts the device's pipeline cache from a file's bytes (empty: no file). A file of another
    // device, driver or shader set, or a damaged one, leaves the cache empty (logged). Before any
    // pipeline is created. False when the cache starts empty.
    bool LoadPipelineCache(plume::RenderDevice& device, const std::vector<uint8_t>& file, uint64_t shaderHash);

    // The bytes of a file holding the device's pipeline cache as it is now; empty when the device has
    // none. Any thread, also while others create pipelines.
    std::vector<uint8_t> SavePipelineCache(plume::RenderDevice& device, uint64_t shaderHash);

    // Constant buffers the shaders of a pipeline read by reference, in order: the root constant
    // buffers b0, b1... of `space` on D3D12; on Vulkan, which has no root descriptors, their device
    // addresses, 8 bytes each, in one push constant range of `stages` (the buffers must be created
    // with CONSTANT and DEVICE_ADDRESSABLE).
    struct ConstantReferences
    {
        uint32_t count = 0;
        uint32_t space = 0;
        plume::RenderShaderStageFlags stages = plume::RenderShaderStageFlag::NONE;
    };

    // A pipeline layout of desc (which has no push constant ranges) plus the references.
    std::unique_ptr<plume::RenderPipelineLayout> CreateLayout(plume::RenderDevice& device,
        plume::RenderPipelineLayoutDesc desc, const ConstantReferences& references);

    // Binds count references on a list whose pipeline layout came from CreateLayout with as many.
    // bound: what the last call on this list bound (count of them), or null when nothing is known;
    // only what differs is set again.
    void BindConstantReferences(plume::RenderCommandList& list, const plume::RenderBufferReference* references,
        uint32_t count, const plume::RenderBufferReference* bound);

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
    // the blend constant color and the stencil reference. On Vulkan the pipelines must declare them
    // dynamic, and binding a pipeline that does not (another pass) drops the values.
    void SetBlendConstants(plume::RenderCommandList& list, const std::array<float, 4>& color);
    void SetStencilReference(plume::RenderCommandList& list, uint32_t reference);

    // The swap chain of a window, with the API's frame latency signal for WaitForFrameLatency: the
    // waitable object on D3D12, present wait on Vulkan when the device has it. Its images are there
    // once resize() succeeds (at once on D3D12).
    std::unique_ptr<plume::RenderSwapChain> CreateSwapChain(plume::RenderCommandQueue& queue, plume::RenderWindow window,
        plume::RenderFormat format, uint32_t textureCount, uint32_t frameLatency);

    // Whether the images of the swap chain are acquired and presented through semaphores (Vulkan):
    // acquireTexture signals one, which the work drawing into the image waits for, and that work
    // signals one, which the present waits for. D3D12 orders both on the queue and takes none.
    bool UsesPresentSemaphores(const plume::RenderSwapChain& swapChain);

    // Whether a present can hold its image for a number of vertical blanks (D3D12's sync interval).
    // A Vulkan present holds it until the next image, and the swap chain's mode (from the vsync
    // setting, at its creation) decides whether it waits for a vertical blank.
    bool HasSyncInterval(const plume::RenderSwapChain& swapChain);

    // How a wait for the swap chain to accept another frame ended.
    enum class LatencyWait
    {
        Ready,       // a present left the queue
        TimedOut,
        Unavailable, // the swap chain has no frame latency signal
    };

    // Waits up to timeoutMs for one present to leave the swap chain's queue (shown or replaced). Each
    // Ready answers one present. On Vulkan it waits for the last present to be shown, so the presents
    // before it answer at once.
    LatencyWait WaitForFrameLatency(plume::RenderSwapChain& swapChain, uint32_t timeoutMs);

    // Presents the image acquired as index, held for syncInterval vertical blanks (0: at once) where
    // the swap chain has a sync interval. allowTearing lets an immediate present tear when the swap
    // chain supports it. rendered: the semaphore the work that drew the image signals, with
    // UsesPresentSemaphores (otherwise null). False when the API refuses the present.
    bool PresentSwapChain(plume::RenderSwapChain& swapChain, uint32_t index, uint32_t syncInterval, bool allowTearing,
        plume::RenderCommandSemaphore* rendered);
}
