// Host test of the check of the SPIR-V contract (vulkan/shader_contract.h) on
// the device Plume creates. Never enters a guest function. The contract of the
// embedded package reads and the device has everything it asks; a copy with
// requirements the device or the backend cannot meet (an unknown capability
// and extension, a uniform buffer over the device's range, an unknown binding
// type and storage format, push constants over the device's size, more sets
// than it binds) names each of them; malformed lines and an index without a
// contract are refused.
// Usage: native_engine_shader_contract_test <new output.jsonl>
#include "gpu/backend/vulkan/shader_contract.h"
#include "gpu/shaders/package.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace plume {
std::unique_ptr<RenderInterface> CreateVulkanInterface();
}

using namespace gpu;
using namespace gpu::vulkan;

namespace {
std::string Json(const std::vector<std::string>& names) {
  std::string out = "[";
  for (const auto& name : names) out += (out.size() > 1 ? ",\"" : "\"") + name + "\"";
  return out + "]";
}
bool Contains(const std::vector<std::string>& names, const std::string& name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 || std::filesystem::exists(argv[1])) return 2;
  std::ofstream output(argv[1]);
  if (!output) return 3;
  bool passed = true;
  auto line = [&](const char* kind, bool ok, const std::string& evidence) {
    passed = passed && ok;
    output << "{\"kind\":\"" << kind << "\",\"ok\":" << (ok ? "true" : "false") << ",\"evidence\":"
           << (evidence.empty() ? "null" : evidence) << "}\n";
  };
  auto render = plume::CreateVulkanInterface();
  auto device = render ? render->createDevice("") : nullptr;
  line("device", device != nullptr, "null");
  if (!device) return 4;
  const auto& vulkan = static_cast<const plume::VulkanDevice&>(*device);

  std::string error;
  const ShaderPackage* package = EmbeddedShaderPackage(ShaderFormat::kSpirv, error);
  ShaderContract contract;
  const bool read = package && ReadShaderContract(package->index, contract, error);
  line("embedded-contract-read", read && contract.capabilities.size() == 7 && contract.bindings.size() == 14,
       "{\"sha256\":\"" + contract.sha256 + "\",\"error\":\"" + error + "\"}");
  const auto missing = MissingFromDevice(contract, vulkan);
  line("embedded-contract-met", read && missing.empty(), "{\"missing\":" + Json(missing) + "}");

  ShaderContract unmet = contract;
  const auto& limits = vulkan.physicalDeviceProperties.limits;
  unmet.capabilities.push_back("Float16");
  unmet.extensions.push_back("SPV_KHR_unknown_for_test");
  const std::string over_range = std::to_string(uint64_t(limits.maxUniformBufferRange) + 4);
  unmet.bindings.push_back({"present", 0, 5, "uniform-buffer", over_range});
  unmet.bindings.push_back({"present", 0, 6, "texel-buffer", "-"});
  unmet.bindings.push_back({"fxaa", 0, 7, "storage-image", "R11fG11fB10f"});
  unmet.bindings.push_back({"wide", limits.maxBoundDescriptorSets, 0, "sampled-image", "-"});
  unmet.push_ranges.push_back({"effect", "fragment", 0, limits.maxPushConstantsSize + 4});
  const auto named = MissingFromDevice(unmet, vulkan);
  const bool all_named = named.size() == 7 && Contains(named, "capability:Float16:unmapped") &&
                         Contains(named, "extension:SPV_KHR_unknown_for_test:unmapped") &&
                         Contains(named, "binding:present:0:5:uniform-range") &&
                         Contains(named, "binding:present:0:6:type-unmapped") &&
                         Contains(named, "binding:fxaa:0:7:storage-format-unmapped") &&
                         Contains(named, "layout:wide:descriptor-sets") && Contains(named, "push:effect:fragment");
  line("unmet-contract-named", all_named, "{\"missing\":" + Json(named) + "}");

  ShaderContract refused;
  const std::string feature = "contract\t" + contract.sha256 + "\nrequire\tfeature\tX\n";
  const bool malformed = !ReadShaderContract(feature, refused, error) && error == "contract-require-line";
  const bool bad_binding = !ReadShaderContract("binding\tdraw\tzero\t0\tsampler[]\t-\n", refused, error) &&
                           error == "contract-binding-line";
  const bool absent = !ReadShaderContract("format\t4\nspirv\tembedded\n", refused, error) && error == "contract-absent";
  line("malformed-refused", malformed && bad_binding && absent,
       "{\"malformed\":" + std::string(malformed ? "true" : "false") + ",\"bad_binding\":" +
           (bad_binding ? "true" : "false") + ",\"absent\":" + (absent ? "true" : "false") + "}");
  output << "{\"kind\":\"verdict\",\"passed\":" << (passed ? "true" : "false") << ",\"guest_executed\":false}\n";
  return passed && output.good() ? 0 : 1;
}
