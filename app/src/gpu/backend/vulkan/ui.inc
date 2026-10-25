// The UI layer of the Present (UiFrame, contracts.h; ui_pass.hlsl). Included
// after Backend::State, inside gpu::vulkan. Each UI texture has its view and
// descriptor set (texture 0 is white); the frame's vertices and indices go into
// the batch's upload memory, and its draws into the back buffer in a render
// pass that keeps the image (LOAD).
namespace {
constexpr Use kUiRead{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_SHADER_READ_BIT};
}

// The sampler, the set and pipeline layouts, the white texture and the
// pipeline, each once, so a failed attempt resumes where it stopped. In the
// open batch (the white texture's copy).
bool Backend::State::EnsureUiLayer(std::string& evidence) {
  if (ui_pipeline) return true;
  VkResult result = VK_SUCCESS;
  if (!ui_sampler) {
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.minFilter = info.magFilter = VK_FILTER_LINEAR; info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxLod = VK_LOD_CLAMP_NONE;
    if ((result = vkCreateSampler(vulkan->vk, &info, nullptr, &ui_sampler)) != VK_SUCCESS)
      return GraphicsError(evidence, "native-ui-sampler", result);
  }
  if (!ui_set) {
    const VkDescriptorSetLayoutBinding bindings[]{
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = uint32_t(std::size(bindings)); info.pBindings = bindings;
    if ((result = vkCreateDescriptorSetLayout(vulkan->vk, &info, nullptr, &ui_set)) != VK_SUCCESS)
      return GraphicsError(evidence, "native-ui-set-layout", result);
  }
  if (!ui_layout) {
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, 8};  // The inverse coordinate space.
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = 1; info.pSetLayouts = &ui_set;
    info.pushConstantRangeCount = 1; info.pPushConstantRanges = &push;
    if ((result = vkCreatePipelineLayout(vulkan->vk, &info, nullptr, &ui_layout)) != VK_SUCCESS)
      return GraphicsError(evidence, "native-ui-layout", result);
  }
  if (!ui_textures.contains(0)) {
    const uint8_t white[4]{0xFF, 0xFF, 0xFF, 0xFF};
    if (!WriteUiTexture(0, 1, 1, white, evidence)) return false;
  }
  const auto vs = PassShader("ui_vs", ShaderStage::kVertex), ps = PassShader("ui_ps", ShaderStage::kPixel);
  if (!vs.Present() || !ps.Present()) return GraphicsError(evidence, "native-ui-shader-module");
  auto pipeline = std::make_shared<GraphicsPipeline>(); pipeline->device = vulkan->vk;
  if ((result = MakeRenderPass(vulkan->vk, VK_FORMAT_B8G8R8A8_UNORM, false, 1, pipeline->pass)) != VK_SUCCESS)
    return GraphicsError(evidence, "native-ui-render-pass", result);
  VkShaderModule modules[2]{};
  VkPipelineShaderStageCreateInfo stages[2]{};
  for (uint32_t i = 0; i < 2 && result == VK_SUCCESS; ++i) {
    const auto& bytes = *(i ? ps.bytes : vs.bytes);
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes.size(); info.pCode = reinterpret_cast<const uint32_t*>(bytes.data());
    result = vkCreateShaderModule(vulkan->vk, &info, nullptr, &modules[i]);
    stages[i] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
    stages[i].module = modules[i]; stages[i].pName = "main";
  }
  if (result == VK_SUCCESS) {
    // POSITION, TEXCOORD and COLOR at locations 0 to 2 (-fvk-stage-io-order=decl).
    const VkVertexInputBindingDescription binding{0, uint32_t(sizeof(UiVertex)), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[]{
        {0, 0, VK_FORMAT_R32G32_SFLOAT, uint32_t(offsetof(UiVertex, x))},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, uint32_t(offsetof(UiVertex, u))},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, uint32_t(offsetof(UiVertex, color))}};
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    input.vertexBindingDescriptionCount = 1; input.pVertexBindingDescriptions = &binding;
    input.vertexAttributeDescriptionCount = uint32_t(std::size(attributes)); input.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE; raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState attachment{};  // Straight alpha.
    attachment.blendEnable = VK_TRUE;
    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.colorBlendOp = attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    attachment.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1; blend.pAttachments = &attachment;
    const VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = uint32_t(std::size(states)); dynamic.pDynamicStates = states;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2; info.pStages = stages; info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly; info.pViewportState = &viewport; info.pRasterizationState = &raster;
    info.pMultisampleState = &ms; info.pColorBlendState = &blend; info.pDynamicState = &dynamic;
    info.layout = ui_layout; info.renderPass = pipeline->pass;
    result = vkCreateGraphicsPipelines(vulkan->vk, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline->pipeline);
  }
  for (auto module : modules) if (module) vkDestroyShaderModule(vulkan->vk, module, nullptr);
  if (result != VK_SUCCESS) return GraphicsError(evidence, "native-ui-pipeline", result);
  ui_pipeline = std::move(pipeline);
  return true;
}

// A texture at a new size is a new image, view and set: the old ones are
// retired while submitted work can still read them. At the same size the copy
// goes into the image, after the draws recorded before it (the barrier orders
// them). In the open batch.
bool Backend::State::WriteUiTexture(uint32_t key, uint32_t width, uint32_t height, const uint8_t* rgba,
                                    std::string& evidence) {
  const VkDeviceSize bytes = VkDeviceSize(width) * height * 4;
  UploadPage* page{}; VkDeviceSize offset{}; VkResult result = VK_SUCCESS;
  if (!Upload(bytes, page, offset, result)) return GraphicsError(evidence, "native-ui-texture-upload", result);
  std::memcpy(page->buffer->mapped + offset, rgba, size_t(bytes));
  auto& t = ui_textures[key];
  if (t.image && (t.width != width || t.height != height)) {
    if (InFlight(t.last_use)) retired_ui_textures.push_back(std::move(t));
    t = {};
  }
  if (!t.image) {
    const auto fail = [&](const char* reason) { ui_textures.erase(key); return GraphicsError(evidence, reason, result); };
    if ((result = CreateImage(vulkan->allocator, VK_FORMAT_R8G8B8A8_UNORM, width, height, 1, false, t.image)) != VK_SUCCESS)
      return fail("native-ui-texture-create");
    t.objects = std::make_shared<GraphicsResources>(); t.objects->device = vulkan->vk;
    VkImageView view{};
    if ((result = t.objects->View(t.image->image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1, false,
                                  kIdentityMapping, view)) != VK_SUCCESS)
      return fail("native-ui-texture-view");
    const VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1; pool.poolSizeCount = uint32_t(std::size(sizes)); pool.pPoolSizes = sizes;
    if ((result = vkCreateDescriptorPool(vulkan->vk, &pool, nullptr, &t.objects->pool)) != VK_SUCCESS)
      return fail("native-ui-texture-pool");
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = t.objects->pool; alloc.descriptorSetCount = 1; alloc.pSetLayouts = &ui_set;
    if ((result = vkAllocateDescriptorSets(vulkan->vk, &alloc, &t.set)) != VK_SUCCESS)
      return fail("native-ui-texture-set");
    const VkDescriptorImageInfo image{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo sampler{ui_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet writes[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    writes[0].dstSet = writes[1].dstSet = t.set; writes[0].descriptorCount = writes[1].descriptorCount = 1;
    writes[0].dstBinding = 0; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; writes[0].pImageInfo = &image;
    writes[1].dstBinding = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER; writes[1].pImageInfo = &sampler;
    vkUpdateDescriptorSets(vulkan->vk, 2, writes, 0, nullptr);
    t.width = width; t.height = height;
  }
  batch.list->setFramebuffer(nullptr);
  const auto commands = Commands();
  // Every texel is written: the previous content is not kept.
  Transition(commands, t.image->image, VK_IMAGE_ASPECT_COLOR_BIT, t.state, kCopyDest, true);
  VkBufferImageCopy copy{};
  copy.bufferOffset = offset;
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {width, height, 1};
  vkCmdCopyBufferToImage(commands, page->buffer->buffer, t.image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  Transition(commands, t.image->image, VK_IMAGE_ASPECT_COLOR_BIT, t.state, kUiRead);
  t.last_use = batch.sequence;
  ++ui_texture_uploads;
  return true;
}

// The frame's draws into the back buffer, after the image pass; each draw binds
// its texture's set and its scissor rect in pixels (UiScissorPixels).
bool Backend::State::DrawUi(BackImage& back, std::string& evidence) {
  const UiFrame& f = ui_frame;
  const VkDeviceSize vertex_bytes = f.vertices.size() * sizeof(UiVertex);
  const VkDeviceSize index_bytes = f.indices.size() * sizeof(uint16_t);
  UploadPage* page{}; VkDeviceSize offset{}; VkResult result = VK_SUCCESS;
  if (!Upload(vertex_bytes + index_bytes, page, offset, result))
    return GraphicsError(evidence, "native-ui-frame-upload", result);
  std::memcpy(page->buffer->mapped + offset, f.vertices.data(), size_t(vertex_bytes));
  std::memcpy(page->buffer->mapped + offset + vertex_bytes, f.indices.data(), size_t(index_bytes));
  auto objects = std::make_shared<GraphicsResources>(); objects->device = vulkan->vk;
  batch.retained.push_back(objects); batch.retained.push_back(ui_pipeline);
  auto* p = static_cast<GraphicsPipeline*>(ui_pipeline.get());
  const uint32_t width = swap_extent.width, height = swap_extent.height;
  VkImageView target{};
  if ((result = objects->View(back.image, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1, false,
                              kIdentityMapping, target)) != VK_SUCCESS ||
      (result = objects->Framebuffer(p->pass, {target}, width, height)) != VK_SUCCESS)
    return GraphicsError(evidence, "native-ui-framebuffer", result);
  batch.list->setFramebuffer(nullptr);
  const auto commands = Commands();
  // The barriers come before the render pass.
  for (const UiDraw& d : f.draws)
    if (auto it = ui_textures.find(d.texture); it != ui_textures.end())
      Transition(commands, it->second.image->image, VK_IMAGE_ASPECT_COLOR_BIT, it->second.state, kUiRead);
  Transition(commands, back.image, VK_IMAGE_ASPECT_COLOR_BIT, back.state, kColorTarget);
  VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  begin.renderPass = p->pass; begin.framebuffer = objects->framebuffer; begin.renderArea.extent = {width, height};
  vkCmdBeginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
  const VkViewport viewport{0, 0, float(width), float(height), 0, 1};
  vkCmdSetViewport(commands, 0, 1, &viewport);
  vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
  const float inverse_space[2]{1.0f / f.width, 1.0f / f.height};
  vkCmdPushConstants(commands, ui_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(inverse_space), inverse_space);
  vkCmdBindVertexBuffers(commands, 0, 1, &page->buffer->buffer, &offset);
  vkCmdBindIndexBuffer(commands, page->buffer->buffer, offset + vertex_bytes, VK_INDEX_TYPE_UINT16);
  VkDescriptorSet bound{};
  for (const UiDraw& d : f.draws) {
    if (!d.index_count) continue;
    auto it = ui_textures.find(d.texture);
    if (it == ui_textures.end()) { ++ui_draws_skipped; continue; }
    std::array<uint32_t, 4> rect{};
    if (!UiScissorPixels(d, f, width, height, rect)) continue;
    const VkRect2D scissor{{int32_t(rect[0]), int32_t(rect[1])}, {rect[2] - rect[0], rect[3] - rect[1]}};
    vkCmdSetScissor(commands, 0, 1, &scissor);
    if (it->second.set != bound) {
      vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, ui_layout, 0, 1, &it->second.set, 0, nullptr);
      bound = it->second.set;
    }
    vkCmdDrawIndexed(commands, d.index_count, 1, d.index_offset, int32_t(d.vertex_offset), 0);
    it->second.last_use = batch.sequence;
  }
  vkCmdEndRenderPass(commands);
  ++ui_frames_drawn;
  return true;
}

// Once every submitted operation completed (Shutdown).
void Backend::State::ReleaseUi() {
  ui_textures.clear(); retired_ui_textures.clear(); ui_frame = {}; ui_pipeline.reset();
  if (ui_layout) vkDestroyPipelineLayout(vulkan->vk, ui_layout, nullptr);
  if (ui_set) vkDestroyDescriptorSetLayout(vulkan->vk, ui_set, nullptr);
  if (ui_sampler) vkDestroySampler(vulkan->vk, ui_sampler, nullptr);
  ui_layout = VK_NULL_HANDLE; ui_set = VK_NULL_HANDLE; ui_sampler = VK_NULL_HANDLE;
}

bool Backend::UploadUiTexture(uint32_t key, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba,
                              std::string& evidence) {
  auto& s = *state_;
  if (!s.ready || s.owner_thread != GetCurrentThreadId() || s.operation_poisoned || s.unproved_execution)
    return GraphicsError(evidence, "native-ui-owner-or-state");
  const char* reason = nullptr; VkResult result = VK_SUCCESS;
  if (!s.OpenBatch(reason, result)) return GraphicsError(evidence, reason, result);
  if (!s.EnsureUiLayer(evidence) || !s.WriteUiTexture(key, width, height, rgba.data(), evidence)) return false;
  const uint64_t batch_operations = ++s.batch.operations;
  ++s.operations;
  if (batch_operations >= kBatchOperationsMax && !s.CloseBatch(reason, result)) return GraphicsError(evidence, reason, result);
  evidence = fmt::format("{{\"uploaded\":true,\"key\":{},\"size\":[{},{}],\"fence\":{}}}", key, width, height,
                         s.ui_textures[key].last_use);
  return true;
}

bool Backend::ReleaseUiTexture(uint32_t key, std::string& evidence) {
  auto& s = *state_;
  if (!s.ready || s.owner_thread != GetCurrentThreadId()) return GraphicsError(evidence, "native-ui-release-owner");
  // Key 0 (the white texture) belongs to the layer.
  auto it = key ? s.ui_textures.find(key) : s.ui_textures.end();
  if (it == s.ui_textures.end()) {
    evidence = fmt::format("{{\"released\":true,\"key\":{},\"ui_texture\":false}}", key);
    return true;
  }
  const uint64_t last_use = it->second.last_use;
  const bool in_flight = s.InFlight(last_use);
  if (in_flight) s.retired_ui_textures.push_back(std::move(it->second));
  s.ui_textures.erase(it);
  evidence = fmt::format("{{\"released\":true,\"key\":{},\"ui_texture\":true,\"deferred\":{},\"last_use_fence\":{}}}",
                         key, in_flight, last_use);
  return true;
}

bool Backend::SetUiFrame(UiFrame frame, std::string& evidence) {
  auto& s = *state_;
  if (!s.ready || s.owner_thread != GetCurrentThreadId()) return GraphicsError(evidence, "native-ui-frame-owner");
  s.ui_frame = std::move(frame);
  evidence = fmt::format("{{\"set\":true,\"vertices\":{},\"indices\":{},\"draws\":{}}}", s.ui_frame.vertices.size(),
                         s.ui_frame.indices.size(), s.ui_frame.draws.size());
  return true;
}
