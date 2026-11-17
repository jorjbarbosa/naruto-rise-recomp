// The graphics API under the renderer: Plume's interface for D3D12 or Vulkan, the API's debug setup,
// and the check that a device can run the renderer.

#include "gpu/renderer/backend.h"

#include "gpu/renderer/descriptors.h"
#include "log.h"

#include <plume_d3d12.h>
#include <plume_vulkan.h>

#include <iterator>

namespace plume
{
    std::unique_ptr<RenderInterface> CreateD3D12Interface();
    std::unique_ptr<RenderInterface> CreateVulkanInterface();
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
            if (settings.debugLayer)
                Log("gpu: Vulkan validation layer unavailable");
            api = plume::CreateVulkanInterface();
        }
        else
        {
            if (settings.debugLayer)
                EnableD3D12DebugLayer();
            api = plume::CreateD3D12Interface();
        }
        if (!api)
            Log("gpu: error: cannot create the %s interface", ApiName(settings.api));
        return api;
    }

    bool PrepareDevice(plume::RenderDevice& device, const Settings& settings)
    {
        if (settings.api == GpuApi::Vulkan)
            return CheckVulkanDevice(device);
        if (settings.debugLayer)
            FilterD3D12DebugMessages(device);
        return true;
    }
}
