#include "graphics_devices.h"

#include <algorithm>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>
#endif
#if defined(NARUTORISE_MENU_NATIVE_VULKAN)
#include <SDL3/SDL_loadso.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif

const std::vector<std::string>& GraphicsDevices(bool vulkan) {
#if defined(NARUTORISE_MENU_NATIVE_VULKAN)
  if (vulkan) {
    static const std::vector<std::string> devices = [] {
      std::vector<std::string> names;
#ifdef _WIN32
      const char* library = "vulkan-1.dll";
#else
      const char* library = "libvulkan.so.1";
#endif
      auto* loader = SDL_LoadObject(library);
      if (!loader) return names;
      auto proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_LoadFunction(loader, "vkGetInstanceProcAddr"));
      auto create = proc ? reinterpret_cast<PFN_vkCreateInstance>(proc(nullptr, "vkCreateInstance")) : nullptr;
      VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_2;
      VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; info.pApplicationInfo = &app;
      VkInstance instance{};
      if (create && create(&info, nullptr, &instance) == VK_SUCCESS) {
        auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(proc(instance, "vkEnumeratePhysicalDevices"));
        auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(proc(instance, "vkGetPhysicalDeviceProperties"));
        auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(proc(instance, "vkDestroyInstance"));
        uint32_t count = 0;
        if (enumerate(instance, &count, nullptr) == VK_SUCCESS) {
          std::vector<VkPhysicalDevice> physical(count);
          if (enumerate(instance, &count, physical.data()) == VK_SUCCESS) for (auto device : physical) {
            VkPhysicalDeviceProperties p{}; properties(device, &p);
            if (p.apiVersion >= VK_API_VERSION_1_2 &&
                std::find(names.begin(), names.end(), p.deviceName) == names.end()) names.emplace_back(p.deviceName);
          }
        }
        destroy(instance, nullptr);
      }
      SDL_UnloadObject(loader); return names;
    }();
    return devices;
  }
#endif
  static const std::vector<std::string> devices = [] {
    std::vector<std::string> names;
#ifdef _WIN32
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return names;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) == S_OK; ++index) {
      DXGI_ADAPTER_DESC1 desc;
      char name[256];
      // As the game: no software or remote adapter, Direct3D 12 at feature level 11_0.
      if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
          !(desc.Flags & (DXGI_ADAPTER_FLAG_SOFTWARE | DXGI_ADAPTER_FLAG_REMOTE)) &&
          SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)) &&
          WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr) > 1 &&
          std::find(names.begin(), names.end(), name) == names.end()) {
        names.emplace_back(name);
      }
      adapter->Release();
    }
    factory->Release();
#endif
    return names;
  }();
  return devices;
}
