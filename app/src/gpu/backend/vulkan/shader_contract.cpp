// The SPIR-V contract of the build against the device (shader_contract.h).
#include "gpu/backend/vulkan/shader_contract.h"

#include <algorithm>
#include <charconv>
#include <map>
#include <string_view>

namespace gpu::vulkan {
namespace {
std::vector<std::string_view> Fields(std::string_view line) {
  std::vector<std::string_view> fields;
  for (size_t at = 0;;) {
    const size_t tab = line.find('\t', at);
    fields.push_back(line.substr(at, tab == std::string_view::npos ? std::string_view::npos : tab - at));
    if (tab == std::string_view::npos) return fields;
    at = tab + 1;
  }
}
bool Number(std::string_view text, uint32_t& value) {
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc() && end == text.data() + text.size() && !text.empty();
}
}  // namespace

bool ReadShaderContract(const std::string& index, ShaderContract& contract, std::string& error) {
  contract = {};
  std::string_view text(index);
  while (!text.empty()) {
    const size_t end = text.find('\n');
    std::string_view line = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);  // The index is written with CRLF on Windows.
    const auto f = Fields(line);
    if (f[0] == "contract" && f.size() == 2) {
      contract.sha256 = f[1];
    } else if (f[0] == "require") {
      if (f.size() != 3 || (f[1] != "capability" && f[1] != "extension")) {
        error = "contract-require-line";
        return false;
      }
      (f[1] == "capability" ? contract.capabilities : contract.extensions).emplace_back(f[2]);
    } else if (f[0] == "binding") {
      ShaderContract::Binding b;
      if (f.size() != 6 || !Number(f[2], b.set) || !Number(f[3], b.binding)) {
        error = "contract-binding-line";
        return false;
      }
      b.layout = f[1];
      b.type = f[4];
      b.detail = f[5];
      contract.bindings.push_back(std::move(b));
    } else if (f[0] == "push") {
      ShaderContract::PushRange r;
      if (f.size() != 5 || !Number(f[3], r.begin) || !Number(f[4], r.end) || r.end < r.begin) {
        error = "contract-push-line";
        return false;
      }
      r.layout = f[1];
      r.stage = f[2];
      contract.push_ranges.push_back(std::move(r));
    }
  }
  if (contract.sha256.size() != 64 || contract.capabilities.empty() || contract.bindings.empty()) {
    error = "contract-absent";
    return false;
  }
  return true;
}

std::vector<std::string> MissingFromDevice(const ShaderContract& contract, const plume::VulkanDevice& device) {
  std::vector<std::string> missing;
  // Plume enables every core feature the device supports, and the descriptor
  // indexing and buffer device address features when it supports them
  // (capabilities).
  VkPhysicalDeviceFeatures features{};
  vkGetPhysicalDeviceFeatures(device.physicalDevice, &features);
  const plume::RenderDeviceCapabilities& enabled = device.capabilities;
  const VkPhysicalDeviceLimits& limits = device.physicalDeviceProperties.limits;
  for (const auto& name : contract.capabilities) {
    bool has = false;
    // Core, no feature: Vulkan 1.0 supports them unconditionally (the SPIR-V
    // environment of the specification). DerivativeControl: ddx_coarse and
    // ddy_coarse, the getGradients of the translated cel shaders.
    if (name == "Shader" || name == "ImageQuery" || name == "DerivativeControl") has = true;
    else if (name == "ClipDistance") has = features.shaderClipDistance;
    else if (name == "Int64") has = features.shaderInt64;
    else if (name == "StorageImageExtendedFormats") has = features.shaderStorageImageExtendedFormats;
    else if (name == "PhysicalStorageBufferAddresses") has = enabled.bufferDeviceAddress;
    else if (name == "RuntimeDescriptorArray") has = enabled.descriptorIndexing;
    else {
      missing.push_back("capability:" + name + ":unmapped");
      continue;
    }
    if (!has) missing.push_back("capability:" + name);
  }
  for (const auto& name : contract.extensions) {
    if (name != "SPV_EXT_descriptor_indexing") missing.push_back("extension:" + name + ":unmapped");
    else if (!enabled.descriptorIndexing) missing.push_back("extension:" + name);
  }
  std::map<std::string, uint32_t> sets;  // Per layout, the sets it binds.
  for (const auto& b : contract.bindings) {
    sets[b.layout] = std::max(sets[b.layout], b.set + 1);
    const std::string name = "binding:" + b.layout + ":" + std::to_string(b.set) + ":" + std::to_string(b.binding);
    if (b.type.ends_with("[]")) {
      // The arrays of the draw layout, indexed by the shaders (bindless).
      if (!enabled.descriptorIndexing) missing.push_back(name + ":descriptor-indexing");
    } else if (b.type == "uniform-buffer") {
      uint32_t bytes = 0;
      if (!Number(b.detail, bytes) || bytes > limits.maxUniformBufferRange) missing.push_back(name + ":uniform-range");
    } else if (b.type == "storage-image") {
      // The SPIR-V image formats of the passes, as Vulkan formats.
      const VkFormat format = b.detail == "Rgb10A2" ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_UNDEFINED;
      VkFormatProperties properties{};
      if (format == VK_FORMAT_UNDEFINED) {
        missing.push_back(name + ":storage-format-unmapped");
        continue;
      }
      vkGetPhysicalDeviceFormatProperties(device.physicalDevice, format, &properties);
      if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
        missing.push_back(name + ":storage-format");
    } else if (b.type != "sampled-image" && b.type != "sampler") {
      missing.push_back(name + ":type-unmapped");
    }
  }
  for (const auto& [layout, count] : sets)
    if (count > limits.maxBoundDescriptorSets) missing.push_back("layout:" + layout + ":descriptor-sets");
  for (const auto& r : contract.push_ranges)
    if (r.end > limits.maxPushConstantsSize) missing.push_back("push:" + r.layout + ":" + r.stage);
  return missing;
}

}  // namespace gpu::vulkan
