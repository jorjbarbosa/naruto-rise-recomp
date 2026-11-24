// The graphics API under the renderer: Plume's interface for D3D12 or Vulkan, the API's debug setup,
// the check that a device can run the renderer, and what the renderer needs past Plume's interface.

#include "gpu/renderer/backend.h"

#include "gpu/renderer/descriptors.h"
#include "gpu/renderer/targets.h"
#include "gpu/textures/formats.h"
#include "log.h"

// D3D12 exists only in Windows builds (ROAN_D3D12): elsewhere every device is a Vulkan one, and each
// D3D12 branch below is left out.
#ifdef ROAN_D3D12
#include <plume_d3d12.h>
#endif
#include <plume_vulkan.h>

#include <cstring>
#include <iterator>
#include <mutex>
#include <unordered_map>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace plume
{
#ifdef ROAN_D3D12
    std::unique_ptr<RenderInterface> CreateD3D12Interface();
#endif
    // Plume's own mapping of its formats, defined in plume_vulkan.cpp.
    VkFormat toVk(RenderFormat format);
}

namespace gpu
{
    namespace
    {
        // The translated shaders write 18 float4 interpolators (locations 0 to 17), and the pixel
        // shaders read up to the last one.
        constexpr uint32_t kInterpolatorComponents = 18 * 4;
        // The user clip planes (PA_CL_UCP), written as SV_ClipDistance.
        constexpr uint32_t kClipDistances = 6;
        // The draw constants of the translated shaders, read by reference in the order of
        // DrawConstants: registers b0 to b2 of space 4 on D3D12, the PushConstants of
        // shader_common.h on Vulkan.
        const ConstantReferences kDrawConstants{ 3, 4,
            plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::PIXEL };
        // The most references a layout has.
        constexpr uint32_t kMaxReferences = 3;

        // A validation message that repeats (every draw, every frame) is logged this many times.
        constexpr uint32_t kValidationRepeats = 3;

        // The header of a pipeline cache file, before the driver's data. Plain data without padding.
        // The driver checks its own header in the data too, but some drivers keep the cache UUID
        // across updates that change the data, hence the driver version and the data's hash.
        constexpr uint32_t kPipelineCacheMagic = 0x43505852; // "RXPC"
        constexpr uint32_t kPipelineCacheVersion = 1;
        struct PipelineCacheHeader
        {
            uint32_t magic = kPipelineCacheMagic;
            uint32_t version = kPipelineCacheVersion;
            uint32_t vendorId = 0;
            uint32_t deviceId = 0;
            uint32_t driverVersion = 0;
            uint32_t dataSize = 0;
            uint8_t cacheUuid[VK_UUID_SIZE] = {};
            uint64_t shaderHash = 0;
            uint64_t dataHash = 0; // XXH3-64 of the data
        };
        static_assert(sizeof(PipelineCacheHeader) == 56, "PipelineCacheHeader has padding");

        // The header of a file the device would take, without the data's size and hash.
        PipelineCacheHeader PipelineCacheIdentity(const plume::VulkanDevice& device, uint64_t shaderHash)
        {
            const VkPhysicalDeviceProperties& properties = device.physicalDeviceProperties;
            PipelineCacheHeader header;
            header.vendorId = properties.vendorID;
            header.deviceId = properties.deviceID;
            header.driverVersion = properties.driverVersion;
            memcpy(header.cacheUuid, properties.pipelineCacheUUID, VK_UUID_SIZE);
            header.shaderHash = shaderHash;
            return header;
        }

        // Why a pipeline cache file does not fit the device, or null when it does.
        const char* PipelineCacheMismatch(const std::vector<uint8_t>& file, const PipelineCacheHeader& expected)
        {
            PipelineCacheHeader header;
            if (file.size() < sizeof(header))
                return "it is too short";
            memcpy(&header, file.data(), sizeof(header));
            if (header.magic != expected.magic || header.version != expected.version)
                return "its format is another";
            if (header.vendorId != expected.vendorId || header.deviceId != expected.deviceId)
                return "it is of another device";
            if (header.driverVersion != expected.driverVersion ||
                memcmp(header.cacheUuid, expected.cacheUuid, VK_UUID_SIZE) != 0)
                return "it is of another driver";
            if (header.shaderHash != expected.shaderHash)
                return "it is of other shaders";
            if (header.dataSize != file.size() - sizeof(header) ||
                XXH3_64bits(file.data() + sizeof(header), header.dataSize) != header.dataHash)
                return "it is damaged";
            return nullptr;
        }

        std::mutex g_validationMutex; // g_validationCounts
        std::unordered_map<int32_t, uint32_t> g_validationCounts; // by message id

        // Vulkan validation messages go to the log, each message id kValidationRepeats times. Any
        // thread that makes a Vulkan call.
        VKAPI_ATTR VkBool32 VKAPI_CALL LogValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
            VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
        {
            uint32_t count;
            {
                std::lock_guard lock(g_validationMutex);
                count = ++g_validationCounts[data->messageIdNumber];
            }
            const char* kind = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0 ? "error" : "warning";
            if (count <= kValidationRepeats)
                Log("gpu: Vulkan validation %s: %s", kind, data->pMessage);
            if (count == kValidationRepeats)
                Log("gpu: Vulkan validation: message %d is not logged again", data->messageIdNumber);
            return VK_FALSE;
        }

#ifdef ROAN_D3D12
        // Turns on the D3D12 debug layer; before Plume creates the device.
        void EnableD3D12DebugLayer()
        {
            ID3D12Debug* debug = nullptr;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            {
                debug->EnableDebugLayer();
                debug->Release();
                Log("gpu: D3D12 debug layer on");
            }
            else
            {
                Log("gpu: D3D12 debug layer unavailable");
            }
        }

        // Debug layer: the guest clears to any value, so the hint that a clear does not match the
        // value given at the target's creation is noise.
        void FilterD3D12DebugMessages(plume::RenderDevice& device)
        {
            ID3D12InfoQueue* queue = nullptr;
            if (FAILED(static_cast<plume::D3D12Device&>(device).d3d->QueryInterface(IID_PPV_ARGS(&queue))))
                return;
            D3D12_MESSAGE_ID ignored[] = { D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
                D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE };
            D3D12_INFO_QUEUE_FILTER filter = {};
            filter.DenyList.NumIDs = UINT(std::size(ignored));
            filter.DenyList.pIDList = ignored;
            queue->AddStorageFilterEntries(&filter);
            queue->Release();
        }
#endif

        // Logs the Vulkan version and driver of the device and checks what the renderer needs. Plume
        // turns on every core feature the device reports, the descriptor indexing features it reports
        // and buffer device addresses when present, so supported here means enabled.
        bool CheckVulkanDevice(plume::RenderDevice& device)
        {
            const auto& vulkan = static_cast<const plume::VulkanDevice&>(device);
            const VkPhysicalDeviceProperties& properties = vulkan.physicalDeviceProperties;
            if (properties.apiVersion < VK_API_VERSION_1_2)
            {
                Log("gpu: error: the Vulkan device supports Vulkan %u.%u; the renderer needs 1.2",
                    VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion));
                return false;
            }

            VkPhysicalDeviceDescriptorIndexingFeatures indexing = {};
            indexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
            VkPhysicalDeviceFeatures2 features = {};
            features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features.pNext = &indexing;
            vkGetPhysicalDeviceFeatures2(vulkan.physicalDevice, &features);

            VkPhysicalDeviceDriverProperties driver = {};
            driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
            VkPhysicalDeviceDescriptorIndexingProperties indexingLimits = {};
            indexingLimits.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES;
            indexingLimits.pNext = &driver;
            VkPhysicalDeviceProperties2 properties2 = {};
            properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties2.pNext = &indexingLimits;
            vkGetPhysicalDeviceProperties2(vulkan.physicalDevice, &properties2);

            Log("gpu: Vulkan %u.%u.%u, driver %s (%s)", VK_API_VERSION_MAJOR(properties.apiVersion),
                VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion),
                driver.driverName, driver.driverInfo);

            bool ok = true;
            auto require = [&](bool supported, const char* name) {
                if (supported)
                    return;
                Log("gpu: error: the Vulkan device lacks %s", name);
                ok = false;
            };
            auto atLeast = [&](uint32_t value, uint32_t needed, const char* name) {
                if (value >= needed)
                    return;
                Log("gpu: error: the Vulkan device has %s %u; the renderer needs %u", name, value, needed);
                ok = false;
            };

            // The translated shaders: 64-bit constant addresses, clip distances, bindless arrays
            // indexed through the shared constants.
            const plume::RenderDeviceCapabilities& capabilities = device.getCapabilities();
            require(capabilities.bufferDeviceAddress, "bufferDeviceAddress");
            require(features.features.shaderInt64, "shaderInt64");
            require(features.features.shaderClipDistance, "shaderClipDistance");
            require(capabilities.descriptorIndexing, "descriptor indexing (runtime arrays, partially bound, "
                "variable count)");
            require(indexing.descriptorBindingSampledImageUpdateAfterBind,
                "descriptorBindingSampledImageUpdateAfterBind");
            // The Xenos mirror-once clamps (descriptors.cpp); Plume has no fallback for them.
            require(capabilities.samplerMirrorClampToEdge, "samplerMirrorClampToEdge");

            const VkPhysicalDeviceLimits& limits = properties.limits;
            atLeast(limits.maxVertexOutputComponents, kInterpolatorComponents, "maxVertexOutputComponents");
            atLeast(limits.maxFragmentInputComponents, kInterpolatorComponents, "maxFragmentInputComponents");
            atLeast(limits.maxClipDistances, kClipDistances, "maxClipDistances");

            // Both stages see the three texture sets and the sampler set.
            uint32_t textures = 0;
            for (uint32_t set = 0; set < Descriptors::kSetCount; set++)
                if (set != Descriptors::kSamplerSet)
                    textures += Descriptors::kCapacity[set];
            uint32_t samplers = Descriptors::kCapacity[Descriptors::kSamplerSet];
            atLeast(indexingLimits.maxPerStageDescriptorUpdateAfterBindSampledImages, textures,
                "maxPerStageDescriptorUpdateAfterBindSampledImages");
            atLeast(indexingLimits.maxDescriptorSetUpdateAfterBindSampledImages, textures,
                "maxDescriptorSetUpdateAfterBindSampledImages");
            atLeast(indexingLimits.maxPerStageDescriptorUpdateAfterBindSamplers, samplers,
                "maxPerStageDescriptorUpdateAfterBindSamplers");
            atLeast(indexingLimits.maxDescriptorSetUpdateAfterBindSamplers, samplers,
                "maxDescriptorSetUpdateAfterBindSamplers");
            atLeast(indexingLimits.maxPerStageUpdateAfterBindResources, textures + samplers,
                "maxPerStageUpdateAfterBindResources");

            // Formats: the guest textures are uploaded and sampled with the guest's filter; the
            // surfaces are drawn into (blended), resolved, copied and sampled by the resolve pass.
            auto format = [&](plume::RenderFormat renderFormat, VkFormatFeatureFlags needed) {
                VkFormatProperties formatProperties = {};
                vkGetPhysicalDeviceFormatProperties(vulkan.physicalDevice, plume::toVk(renderFormat), &formatProperties);
                VkFormatFeatureFlags missing = needed & ~formatProperties.optimalTilingFeatures;
                if (missing == 0)
                    return;
                Log("gpu: error: the Vulkan device lacks features %08X of format %d", missing, int(renderFormat));
                ok = false;
            };
            constexpr VkFormatFeatureFlags kSampled = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            constexpr VkFormatFeatureFlags kCopied = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            for (plume::RenderFormat texture : textures::HostFormats())
                format(texture, kSampled | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
            format(Targets::kColorFormat, kSampled | kCopied | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
            format(Targets::kResolvedDepthFormat, kSampled | kCopied | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
            format(Targets::kDepthFormat, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);

            Log("gpu: present wait %s", capabilities.presentWait ? "available" : "unavailable");
            return ok;
        }
    }

    const char* ApiName(GpuApi api)
    {
        return api == GpuApi::Vulkan ? "Vulkan" : "D3D12";
    }

    std::unique_ptr<plume::RenderInterface> CreateInterface(const Settings& settings)
    {
        std::unique_ptr<plume::RenderInterface> api;
        if (settings.api == GpuApi::Vulkan)
        {
            plume::VulkanValidationDesc validation;
            validation.enabled = settings.debugLayer;
            validation.synchronization = settings.syncValidation;
            validation.callback = LogValidationMessage;
            api = plume::CreateVulkanInterface(validation);
            if (api && settings.debugLayer)
            {
                const auto& vulkan = static_cast<const plume::VulkanInterface&>(*api);
                if (!vulkan.validationEnabled)
                    Log("gpu: Vulkan validation layer unavailable");
                else if (!vulkan.debugUtilsEnabled)
                    Log("gpu: Vulkan validation layer on, without VK_EXT_debug_utils: its messages are not logged");
                else
                    Log("gpu: Vulkan validation layer on%s", vulkan.synchronizationValidationEnabled
                        ? ", with synchronization validation" : "");
                if (settings.syncValidation && vulkan.validationEnabled && !vulkan.synchronizationValidationEnabled)
                    Log("gpu: Vulkan synchronization validation unavailable");
            }
        }
        else
        {
#ifdef ROAN_D3D12
            if (settings.debugLayer)
                EnableD3D12DebugLayer();
            api = plume::CreateD3D12Interface();
#endif
        }
        if (!api)
            Log("gpu: error: cannot create the %s interface", ApiName(settings.api));
        return api;
    }

    bool PrepareDevice(plume::RenderDevice& device, const Settings& settings)
    {
        if (settings.api == GpuApi::Vulkan)
            return CheckVulkanDevice(device);
#ifdef ROAN_D3D12
        if (settings.debugLayer)
            FilterD3D12DebugMessages(device);
#endif
        return true;
    }

    uint32_t SurfaceSampleCounts(plume::RenderDevice& device)
    {
        if (const auto* vulkan = dynamic_cast<const plume::VulkanDevice*>(&device))
        {
            const VkPhysicalDeviceLimits& limits = vulkan->physicalDeviceProperties.limits;
            return limits.framebufferColorSampleCounts & limits.framebufferDepthSampleCounts &
                limits.framebufferStencilSampleCounts & limits.sampledImageColorSampleCounts &
                limits.sampledImageDepthSampleCounts;
        }
        return device.getSampleCountsSupported(Targets::kColorFormat) &
            device.getSampleCountsSupported(Targets::kDepthFormat);
    }

    bool IsCreated(const plume::RenderBuffer* buffer)
    {
#ifdef ROAN_D3D12
        if (const auto* d3d12 = dynamic_cast<const plume::D3D12Buffer*>(buffer))
            return d3d12->d3d != nullptr;
#endif
        const auto* vulkan = dynamic_cast<const plume::VulkanBuffer*>(buffer);
        return vulkan != nullptr && vulkan->vk != VK_NULL_HANDLE;
    }

    bool IsCreated(const plume::RenderTexture* texture)
    {
#ifdef ROAN_D3D12
        if (const auto* d3d12 = dynamic_cast<const plume::D3D12Texture*>(texture))
            return d3d12->d3d != nullptr;
#endif
        const auto* vulkan = dynamic_cast<const plume::VulkanTexture*>(texture);
        return vulkan != nullptr && vulkan->vk != VK_NULL_HANDLE;
    }

    bool IsCreated(const plume::RenderPipeline* pipeline)
    {
#ifdef ROAN_D3D12
        if (const auto* d3d12 = dynamic_cast<const plume::D3D12GraphicsPipeline*>(pipeline))
            return d3d12->d3d != nullptr;
#endif
        const auto* vulkan = dynamic_cast<const plume::VulkanGraphicsPipeline*>(pipeline);
        return vulkan != nullptr && vulkan->vk != VK_NULL_HANDLE;
    }

    bool IsCreated(const plume::RenderSwapChain* swapChain)
    {
#ifdef ROAN_D3D12
        if (const auto* d3d12 = dynamic_cast<const plume::D3D12SwapChain*>(swapChain))
            return d3d12->d3d != nullptr;
#endif
        // A Vulkan swap chain gets its images at its first resize; the surface comes at creation.
        const auto* vulkan = dynamic_cast<const plume::VulkanSwapChain*>(swapChain);
        return vulkan != nullptr && vulkan->surface != VK_NULL_HANDLE;
    }

    plume::RenderShaderFormat ShaderFormat(plume::RenderDevice& device)
    {
#ifdef ROAN_D3D12
        if (dynamic_cast<plume::D3D12Device*>(&device) != nullptr)
            return plume::RenderShaderFormat::DXIL;
#endif
        return plume::RenderShaderFormat::SPIRV;
    }

    std::unique_ptr<plume::RenderShader> CreatePassShader(plume::RenderDevice& device, std::span<const unsigned char> dxil,
        std::span<const unsigned char> spirv)
    {
        if (ShaderFormat(device) == plume::RenderShaderFormat::DXIL)
        {
            if (dxil.empty())
                Fatal("gpu: this build has no DXIL pass shaders");
            return device.createShader(dxil.data(), dxil.size(), "main", plume::RenderShaderFormat::DXIL);
        }
        // The header's bytes have no alignment, and Vulkan reads SPIR-V as 32-bit words.
        std::vector<uint32_t> words((spirv.size() + 3) / 4);
        memcpy(words.data(), spirv.data(), spirv.size());
        return device.createShader(words.data(), spirv.size(), "main", plume::RenderShaderFormat::SPIRV);
    }

    bool KeepsPipelineCache(plume::RenderDevice& device)
    {
        return dynamic_cast<plume::VulkanDevice*>(&device) != nullptr;
    }

    bool LoadPipelineCache(plume::RenderDevice& device, const std::vector<uint8_t>& file, uint64_t shaderHash)
    {
        auto* vulkan = dynamic_cast<plume::VulkanDevice*>(&device);
        if (vulkan == nullptr)
            return false;
        if (file.empty())
        {
            vulkan->resetPipelineCache(nullptr, 0);
            Log("gpu: no pipeline cache yet; it starts empty");
            return false;
        }
        if (const char* mismatch = PipelineCacheMismatch(file, PipelineCacheIdentity(*vulkan, shaderHash)))
        {
            vulkan->resetPipelineCache(nullptr, 0);
            Log("gpu: the pipeline cache file is not used (%s); the cache starts empty", mismatch);
            return false;
        }
        size_t dataSize = file.size() - sizeof(PipelineCacheHeader);
        if (!vulkan->resetPipelineCache(file.data() + sizeof(PipelineCacheHeader), dataSize))
        {
            Log("gpu: the driver refused the pipeline cache file; the cache starts empty");
            return false;
        }
        Log("gpu: pipeline cache of %zu KB loaded", dataSize >> 10);
        return true;
    }

    std::vector<uint8_t> SavePipelineCache(plume::RenderDevice& device, uint64_t shaderHash)
    {
        auto* vulkan = dynamic_cast<plume::VulkanDevice*>(&device);
        if (vulkan == nullptr)
            return {};
        std::vector<uint8_t> data = vulkan->getPipelineCacheData();
        if (data.empty() || data.size() > UINT32_MAX)
            return {};
        PipelineCacheHeader header = PipelineCacheIdentity(*vulkan, shaderHash);
        header.dataSize = uint32_t(data.size());
        header.dataHash = XXH3_64bits(data.data(), data.size());
        std::vector<uint8_t> file(sizeof(header) + data.size());
        memcpy(file.data(), &header, sizeof(header));
        memcpy(file.data() + sizeof(header), data.data(), data.size());
        return file;
    }

    std::unique_ptr<plume::RenderPipelineLayout> CreateLayout(plume::RenderDevice& device,
        plume::RenderPipelineLayoutDesc desc, const ConstantReferences& references)
    {
#ifdef ROAN_D3D12
        if (dynamic_cast<plume::D3D12Device*>(&device) != nullptr)
        {
            plume::RenderRootDescriptorDesc roots[kMaxReferences];
            for (uint32_t i = 0; i < references.count; i++)
                roots[i] = plume::RenderRootDescriptorDesc(i, references.space, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
            desc.rootDescriptorDescs = roots;
            desc.rootDescriptorDescsCount = references.count;
            return device.createPipelineLayout(desc);
        }
#endif
        const plume::RenderPushConstantRange addresses(0, 0, 0, references.count * uint32_t(sizeof(uint64_t)),
            references.stages);
        desc.pushConstantRanges = &addresses;
        desc.pushConstantRangesCount = 1;
        return device.createPipelineLayout(desc);
    }

    void BindConstantReferences(plume::RenderCommandList& list, const plume::RenderBufferReference* references,
        uint32_t count, const plume::RenderBufferReference* bound)
    {
#ifdef ROAN_D3D12
        bool d3d12 = dynamic_cast<plume::D3D12CommandList*>(&list) != nullptr;
#endif
        for (uint32_t i = 0; i < count; i++)
        {
            const plume::RenderBufferReference& reference = references[i];
            if (bound != nullptr && bound[i] == reference)
                continue;
#ifdef ROAN_D3D12
            if (d3d12)
            {
                list.setGraphicsRootDescriptor(reference, i);
                continue;
            }
#endif
            uint64_t address = reference.ref->getDeviceAddress() + reference.offset;
            list.setGraphicsPushConstants(0, &address, i * uint32_t(sizeof(address)), uint32_t(sizeof(address)));
        }
    }

    std::unique_ptr<plume::RenderPipelineLayout> CreateDrawLayout(plume::RenderDevice& device,
        const plume::RenderDescriptorSetDesc* sets, uint32_t setCount)
    {
        return CreateLayout(device, plume::RenderPipelineLayoutDesc(nullptr, 0, sets, setCount, false, true), kDrawConstants);
    }

    void BindDrawConstants(plume::RenderCommandList& list, const DrawConstants& constants,
        const std::optional<DrawConstants>& bound)
    {
        const plume::RenderBufferReference references[] = { constants.vertex, constants.pixel, constants.shared };
        if (!bound)
        {
            BindConstantReferences(list, references, kDrawConstants.count, nullptr);
            return;
        }
        const plume::RenderBufferReference last[] = { bound->vertex, bound->pixel, bound->shared };
        BindConstantReferences(list, references, kDrawConstants.count, last);
    }

    void SetBlendConstants(plume::RenderCommandList& list, const std::array<float, 4>& color)
    {
#ifdef ROAN_D3D12
        if (auto* d3d12 = dynamic_cast<plume::D3D12CommandList*>(&list))
        {
            d3d12->d3d->OMSetBlendFactor(color.data());
            return;
        }
#endif
        static_cast<plume::VulkanCommandList&>(list).setBlendConstants(color.data());
    }

    void SetStencilReference(plume::RenderCommandList& list, uint32_t reference)
    {
#ifdef ROAN_D3D12
        if (auto* d3d12 = dynamic_cast<plume::D3D12CommandList*>(&list))
        {
            d3d12->d3d->OMSetStencilRef(reference);
            return;
        }
#endif
        static_cast<plume::VulkanCommandList&>(list).setStencilReference(reference);
    }

    std::unique_ptr<plume::RenderSwapChain> CreateSwapChain(plume::RenderCommandQueue& queue, plume::RenderWindow window,
        plume::RenderFormat format, uint32_t textureCount, uint32_t frameLatency)
    {
        // D3D12's frame latency object comes with every swap chain, and Plume's present wait there
        // would change how it presents.
        auto* vulkan = dynamic_cast<plume::VulkanCommandQueue*>(&queue);
        bool presentWait = vulkan != nullptr && vulkan->device->capabilities.presentWait;
        return queue.createSwapChain(plume::RenderSwapChainDesc(window, format, textureCount, presentWait, frameLatency));
    }

    bool UsesPresentSemaphores(const plume::RenderSwapChain& swapChain)
    {
        return dynamic_cast<const plume::VulkanSwapChain*>(&swapChain) != nullptr;
    }

    bool HasSyncInterval(const plume::RenderSwapChain& swapChain)
    {
#ifdef ROAN_D3D12
        return dynamic_cast<const plume::D3D12SwapChain*>(&swapChain) != nullptr;
#else
        return false;
#endif
    }

    LatencyWait WaitForFrameLatency(plume::RenderSwapChain& swapChain, uint32_t timeoutMs)
    {
        if (auto* vulkan = dynamic_cast<plume::VulkanSwapChain*>(&swapChain))
        {
            if (!vulkan->desc.enablePresentWait)
                return LatencyWait::Unavailable;
            // Present ids start over with each swap chain (Plume patch 0003); 0: nothing presented yet.
            if (vulkan->currentPresentId == 0 || vulkan->vk == VK_NULL_HANDLE)
                return LatencyWait::Ready;
            VkResult result = vkWaitForPresentKHR(vulkan->commandQueue->device->vk, vulkan->vk, vulkan->currentPresentId,
                uint64_t(timeoutMs) * 1000000);
            // Out of date or lost: there is nothing to wait for until the swap chain is made again.
            return result == VK_TIMEOUT ? LatencyWait::TimedOut : LatencyWait::Ready;
        }
#ifdef ROAN_D3D12
        auto* d3d12 = dynamic_cast<plume::D3D12SwapChain*>(&swapChain);
        if (d3d12 != nullptr && d3d12->waitableObject != nullptr)
            return WaitForSingleObject(d3d12->waitableObject, timeoutMs) == WAIT_TIMEOUT ? LatencyWait::TimedOut
                                                                                        : LatencyWait::Ready;
#endif
        return LatencyWait::Unavailable;
    }

    bool PresentSwapChain(plume::RenderSwapChain& swapChain, uint32_t index, uint32_t syncInterval, bool allowTearing,
        plume::RenderCommandSemaphore* rendered)
    {
#ifdef ROAN_D3D12
        if (auto* d3d12 = dynamic_cast<plume::D3D12SwapChain*>(&swapChain))
        {
            // Tearing needs the swap chain flag, and only an immediate present may tear.
            UINT flags = allowTearing && syncInterval == 0 &&
                    (d3d12->swapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0
                ? DXGI_PRESENT_ALLOW_TEARING : 0;
            return SUCCEEDED(d3d12->d3d->Present(syncInterval, flags));
        }
#endif
        return swapChain.present(index, &rendered, rendered != nullptr ? 1 : 0);
    }
}
