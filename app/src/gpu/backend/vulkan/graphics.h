// Native Vulkan graphics objects. Every transient object is retained by its
// command batch until the completion service proves its last use.
#pragma once
#include "gpu/backend/vulkan/formats.h"
#include "gpu/backend/pipeline_set.h"
#include "gpu/shaders/package.h"
#include <bit>
#include <cstring>
#include <map>
#include <set>
#include <span>
#include <regex>

namespace gpu::vulkan {
struct GraphicsPipeline {
  VkDevice device{};
  VkPipeline pipeline{};
  VkRenderPass pass{};
  ~GraphicsPipeline() {
    if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
    if (pass) vkDestroyRenderPass(device, pass, nullptr);
  }
};
struct GraphicsResources {
  VkDevice device{};
  VkDescriptorPool pool{};
  VkFramebuffer framebuffer{};
  std::vector<VkImageView> views;
  std::vector<VkSampler> samplers;
  ~GraphicsResources() {
    if (framebuffer) vkDestroyFramebuffer(device, framebuffer, nullptr);
    if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
    for (auto view : views) vkDestroyImageView(device, view, nullptr);
    for (auto sampler : samplers) vkDestroySampler(device, sampler, nullptr);
  }
  VkResult View(VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t levels,
                bool cube, const ComponentMapping& mapping, VkImageView& view) {
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image;
    info.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.components = ImageViewMapping(mapping);
    info.subresourceRange = {aspect, 0, levels, 0, cube ? 6u : 1u};
    const auto result = vkCreateImageView(device, &info, nullptr, &view);
    if (result == VK_SUCCESS) views.push_back(view);
    return result;
  }
  VkResult Framebuffer(VkRenderPass pass, const std::vector<VkImageView>& attachments,
                       uint32_t width, uint32_t height) {
    VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    info.renderPass = pass;
    info.attachmentCount = uint32_t(attachments.size());
    info.pAttachments = attachments.data();
    info.width = width; info.height = height; info.layers = 1;
    return vkCreateFramebuffer(device, &info, nullptr, &framebuffer);
  }
};

inline VkResult MakeRenderPass(VkDevice device, VkFormat color, bool depth, uint32_t samples, VkRenderPass& out) {
  std::vector<VkAttachmentDescription> attachments;
  auto add = [&](VkFormat format, VkImageLayout layout) {
    VkAttachmentDescription a{};
    a.format = format;
    a.samples = VkSampleCountFlagBits(samples);
    a.loadOp = a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    a.storeOp = a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.initialLayout = a.finalLayout = layout;
    attachments.push_back(a);
  };
  if (color != VK_FORMAT_UNDEFINED) add(color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  VkAttachmentReference cr{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference dr{uint32_t(attachments.size()), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  if (depth) add(VK_FORMAT_D32_SFLOAT_S8_UINT, dr.layout);
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = color != VK_FORMAT_UNDEFINED ? 1 : 0;
  sub.pColorAttachments = sub.colorAttachmentCount ? &cr : nullptr;
  sub.pDepthStencilAttachment = depth ? &dr : nullptr;
  VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  info.attachmentCount = uint32_t(attachments.size());
  info.pAttachments = attachments.data();
  info.subpassCount = 1; info.pSubpasses = &sub;
  return vkCreateRenderPass(device, &info, nullptr, &out);
}

inline VkFormat InputFormat(VertexFormat f) {
  switch (f) {
    case VertexFormat::kR32G32B32A32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case VertexFormat::kR32G32B32A32Uint: return VK_FORMAT_R32G32B32A32_UINT;
    case VertexFormat::kR32G32B32Float: return VK_FORMAT_R32G32B32_SFLOAT;
    case VertexFormat::kR32G32B32Uint: return VK_FORMAT_R32G32B32_UINT;
    case VertexFormat::kR16G16B16A16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VertexFormat::kR16G16B16A16Unorm: return VK_FORMAT_R16G16B16A16_UNORM;
    case VertexFormat::kR16G16B16A16Uint: return VK_FORMAT_R16G16B16A16_UINT;
    case VertexFormat::kR16G16B16A16Snorm: return VK_FORMAT_R16G16B16A16_SNORM;
    case VertexFormat::kR16G16B16A16Sint: return VK_FORMAT_R16G16B16A16_SINT;
    case VertexFormat::kR32G32Float: return VK_FORMAT_R32G32_SFLOAT;
    case VertexFormat::kR32G32Uint: return VK_FORMAT_R32G32_UINT;
    case VertexFormat::kR8G8B8A8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case VertexFormat::kR8G8B8A8Uint: return VK_FORMAT_R8G8B8A8_UINT;
    case VertexFormat::kR8G8B8A8Snorm: return VK_FORMAT_R8G8B8A8_SNORM;
    case VertexFormat::kR8G8B8A8Sint: return VK_FORMAT_R8G8B8A8_SINT;
    case VertexFormat::kR16G16Float: return VK_FORMAT_R16G16_SFLOAT;
    case VertexFormat::kR16G16Unorm: return VK_FORMAT_R16G16_UNORM;
    case VertexFormat::kR16G16Uint: return VK_FORMAT_R16G16_UINT;
    case VertexFormat::kR16G16Snorm: return VK_FORMAT_R16G16_SNORM;
    case VertexFormat::kR16G16Sint: return VK_FORMAT_R16G16_SINT;
    case VertexFormat::kR32Float: return VK_FORMAT_R32_SFLOAT;
    case VertexFormat::kR32Uint: return VK_FORMAT_R32_UINT;
    default: return VK_FORMAT_UNDEFINED;
  }
}
inline bool ApplyDrawConstants(std::span<const uint8_t> whole, std::span<const uint8_t> delta,
                               std::array<uint8_t, 4096>& block) {
  if (whole.size() == block.size()) {
    std::memcpy(block.data(), whole.data(), block.size());
    return delta.empty();
  }
  if (!whole.empty()) return false;
  size_t at = 0;
  while (at + 4 <= delta.size()) {
    uint16_t first, count;
    std::memcpy(&first, delta.data() + at, 2);
    std::memcpy(&count, delta.data() + at + 2, 2);
    if (!count || size_t(first) + count > 256 || at + 4 + size_t(count) * 16 > delta.size()) return false;
    std::memcpy(block.data() + size_t(first) * 16, delta.data() + at + 4, size_t(count) * 16);
    at += 4 + size_t(count) * 16;
  }
  return at == delta.size();
}
inline VkBlendFactor BlendFactor(uint32_t value, bool alpha, bool& ok) {
  switch (value) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 4: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
    case 5: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
    case 9: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_DST_ALPHA;
    case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12: case 14: return alpha ? VK_BLEND_FACTOR_CONSTANT_ALPHA : VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 13: case 15: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: ok = false; return VK_BLEND_FACTOR_ONE;
  }
}
inline VkBlendOp BlendOp(uint32_t value, bool& ok) {
  constexpr VkBlendOp ops[]{VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX,
                             VK_BLEND_OP_REVERSE_SUBTRACT};
  if (value < std::size(ops)) return ops[value];
  ok = false; return VK_BLEND_OP_ADD;
}
inline VkStencilOpState StencilFace(uint32_t value, uint32_t read, uint32_t write) {
  constexpr VkStencilOp ops[]{VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE,
      VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_DECREMENT_AND_CLAMP, VK_STENCIL_OP_INVERT,
      VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_DECREMENT_AND_WRAP};
  return {ops[(value >> 3) & 7], ops[(value >> 6) & 7], ops[(value >> 9) & 7],
           VkCompareOp(value & 7), read, write, 0};
}
inline NativeResult MakeGraphicsPipeline(VkDevice device, VkPipelineLayout layout, const PipelineDesc& d,
    const std::map<std::string, uint32_t>& locations, PipelineSet::Pipeline& out,
    VkFormat pass_color = VK_FORMAT_UNDEFINED, bool auxiliary = false) {
  const auto& w = d.state;
  if (!d.vs.Present() || d.vs.format != ShaderFormat::kSpirv ||
      (!w[17] && (!d.ps.Present() || d.ps.format != ShaderFormat::kSpirv)))
    return NativeResult::Failed("native-vulkan-shader-format", VK_ERROR_INVALID_SHADER_NV);
  auto pipeline = std::make_shared<GraphicsPipeline>();
  pipeline->device = device;
  VkResult result = MakeRenderPass(device, auxiliary ? pass_color :
      w[18] ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_UNDEFINED, w[9], w[8], pipeline->pass);
  if (result != VK_SUCCESS) return NativeResult::Failed("native-vulkan-render-pass", result);
  std::vector<VkVertexInputAttributeDescription> attributes;
  std::vector<VkVertexInputBindingDescription> bindings;
  std::set<uint32_t> used_locations, slots;
  std::map<uint32_t, uint32_t> decorated;
  std::set<uint32_t> inputs, consumed;
  const auto& code = *d.vs.bytes;
  const auto* words = reinterpret_cast<const uint32_t*>(code.data());
  for (size_t at = 5; at < code.size() / 4;) {
    const uint32_t n = words[at] >> 16, op = words[at] & 65535;
    if (!n || at + n > code.size() / 4) return NativeResult::Failed("native-vulkan-invalid-spirv", VK_ERROR_INVALID_SHADER_NV);
    if (op == 71 && n >= 4 && words[at + 2] == 30) decorated[words[at + 1]] = words[at + 3];
    if (op == 59 && n >= 4 && words[at + 3] == 1) inputs.insert(words[at + 2]);
    at += n;
  }
  for (auto id : inputs) if (decorated.count(id)) consumed.insert(decorated.at(id));
  if (d.elements) for (const auto& e : *d.elements) {
    const auto it = locations.find(std::string(e.semantic) + std::to_string(e.semantic_index));
    if (it != locations.end() && !consumed.count(it->second)) continue;
    if (it == locations.end() || !used_locations.insert(it->second).second)
      return NativeResult::Failed("native-vulkan-vertex-location", VK_ERROR_INITIALIZATION_FAILED);
    const VkFormat format = InputFormat(e.format);
    if (format == VK_FORMAT_UNDEFINED) return NativeResult::Failed("native-vulkan-vertex-format", VK_ERROR_FORMAT_NOT_SUPPORTED);
    attributes.push_back({it->second, e.slot, format, e.offset});
    if (slots.insert(e.slot).second) bindings.push_back({e.slot, 0, VK_VERTEX_INPUT_RATE_VERTEX});
  }
  VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  input.vertexBindingDescriptionCount = uint32_t(bindings.size()); input.pVertexBindingDescriptions = bindings.data();
  input.vertexAttributeDescriptionCount = uint32_t(attributes.size()); input.pVertexAttributeDescriptions = attributes.data();
  VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  if (auxiliary && w[10] == 1) assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL; raster.lineWidth = 1;
  raster.cullMode = w[5] && w[6] ? VK_CULL_MODE_NONE :
      w[5] ? VK_CULL_MODE_FRONT_BIT : w[6] ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
  raster.frontFace = w[7] ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
  raster.depthBiasEnable = w[19] || w[20];
  raster.depthBiasConstantFactor = float(int32_t(w[19]));
  raster.depthBiasSlopeFactor = std::bit_cast<float>(w[20]);
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VkSampleCountFlagBits(w[8]); ms.alphaToCoverageEnable = w[11];
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = w[2]; ds.depthWriteEnable = w[3]; ds.depthCompareOp = VkCompareOp(w[4] & 7);
  ds.stencilTestEnable = w[12]; ds.front = StencilFace(w[13], w[15], w[16]);
  ds.back = StencilFace(w[14], w[15], w[16]);
  bool ok = true;
  const auto bc = w[0];
  VkPipelineColorBlendAttachmentState attachment{};
  attachment.blendEnable = bc != 0x00010001 && !w[17];
  attachment.srcColorBlendFactor = BlendFactor(bc & 31, false, ok);
  attachment.dstColorBlendFactor = BlendFactor((bc >> 8) & 31, false, ok);
  attachment.colorBlendOp = BlendOp((bc >> 5) & 7, ok);
  attachment.srcAlphaBlendFactor = BlendFactor((bc >> 16) & 31, true, ok);
  attachment.dstAlphaBlendFactor = BlendFactor((bc >> 24) & 31, true, ok);
  attachment.alphaBlendOp = BlendOp((bc >> 21) & 7, ok);
  attachment.colorWriteMask = w[17] ? 0 : w[1] & 15;
  if (!ok) return NativeResult::Failed("native-draw-blend-uncovered", VK_ERROR_INITIALIZATION_FAILED);
  VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = w[18] ? 1 : 0; blend.pAttachments = &attachment;
  const VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
      VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
      VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE_EXT, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY_EXT};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = auxiliary ? 2 : uint32_t(std::size(states)); dynamic.pDynamicStates = states;
  VkShaderModule modules[2]{};
  VkPipelineShaderStageCreateInfo stages[2]{};
  VkSpecializationMapEntry entry{0, 0, 4};
  const uint32_t values[]{d.vs.spec_value & d.vs.spec_mask, d.ps.spec_value & d.ps.spec_mask};
  VkSpecializationInfo specs[2]{{1, &entry, 4, &values[0]}, {1, &entry, 4, &values[1]}};
  const uint32_t count = w[17] ? 1 : 2;
  for (uint32_t i = 0; i < count; ++i) {
    const auto& bytes = *(i ? d.ps.bytes : d.vs.bytes);
    // std::vector's storage is aligned for uint32_t; reject malformed modules.
    if (bytes.size() % 4) { result = VK_ERROR_INVALID_SHADER_NV; break; }
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes.size(); info.pCode = reinterpret_cast<const uint32_t*>(bytes.data());
    result = vkCreateShaderModule(device, &info, nullptr, &modules[i]);
    if (result != VK_SUCCESS) break;
    stages[i] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
    stages[i].module = modules[i]; stages[i].pName = "main"; stages[i].pSpecializationInfo = &specs[i];
  }
  if (result == VK_SUCCESS) {
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = count; info.pStages = stages; info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly; info.pViewportState = &viewport; info.pRasterizationState = &raster;
    info.pMultisampleState = &ms; info.pDepthStencilState = &ds; info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic; info.layout = layout; info.renderPass = pipeline->pass;
    result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline->pipeline);
  }
  for (auto module : modules) if (module) vkDestroyShaderModule(device, module, nullptr);
  if (result != VK_SUCCESS) return NativeResult::Failed("native-vulkan-graphics-pipeline", result);
  out = std::move(pipeline);
  return {};
}
}  // namespace gpu::vulkan
