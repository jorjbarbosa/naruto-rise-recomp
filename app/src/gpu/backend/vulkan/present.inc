// Own WSI adapter. Render completion and presentation retirement are distinct:
// submission fences protect batches, maintenance1 present fences protect WSI.
bool Backend::State::WaitPresent(VkFence fence, std::string& evidence) {
  const auto deadline = std::chrono::steady_clock::now() + kWaitBudget;
  VkResult result;
  do {
    result = vkWaitForFences(vulkan->vk, 1, &fence, VK_TRUE, 20'000'000);
    if (result == VK_SUCCESS) return true;
    if (result != VK_TIMEOUT) break;
  } while (std::chrono::steady_clock::now() < deadline);
  Observe(result);
  return GraphicsError(evidence, "native-vulkan-present-retirement-unproved", result);
}
bool Backend::State::RetireSwapchain(std::string& evidence) {
  if (acquire_pending) {
    if (!WaitPresent(acquire_fence, evidence)) return false;
    acquire_pending = false;
  }
  if (!swapchain) return true;
  const uint64_t last = next_sequence - 1;
  if (completion->Completed() < last && !completion->WaitFor(last, kWaitBudget))
    return GraphicsError(evidence, "native-vulkan-swapchain-render-pending", VK_TIMEOUT);
  for (auto& back : back_images) if (back.pending) {
    if (!WaitPresent(back.presented, evidence)) return false;
    back.pending = false;
  }
  Reclaim(completion->Completed()); // Framebuffers and image views first.
  for (auto& back : back_images) {
    if (back.rendered) vkDestroySemaphore(vulkan->vk, back.rendered, nullptr);
    if (back.presented) vkDestroyFence(vulkan->vk, back.presented, nullptr);
  }
  back_images.clear();
  vkDestroySwapchainKHR(vulkan->vk, swapchain, nullptr); swapchain = VK_NULL_HANDLE;
  return true;
}
bool Backend::State::CreateSwapchain(uint32_t width, uint32_t height, std::string& evidence) {
  if (!RetireSwapchain(evidence)) return false;
  VkSurfaceCapabilitiesKHR caps{};
  VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vulkan->physicalDevice, window_surface, &caps);
  if (result != VK_SUCCESS) return GraphicsError(evidence, "native-vulkan-surface-capabilities", result);
  uint32_t count = 0;
  result = vkGetPhysicalDeviceSurfaceFormatsKHR(vulkan->physicalDevice, window_surface, &count, nullptr);
  if (result != VK_SUCCESS || !count) return GraphicsError(evidence, "native-vulkan-surface-formats", result);
  std::vector<VkSurfaceFormatKHR> formats(count);
  if ((result = vkGetPhysicalDeviceSurfaceFormatsKHR(vulkan->physicalDevice, window_surface, &count, formats.data())) != VK_SUCCESS)
    return GraphicsError(evidence, "native-vulkan-surface-formats", result);
  bool bgra = false;
  for (const auto& format : formats) bgra |= format.format == VK_FORMAT_B8G8R8A8_UNORM &&
      format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  if (!bgra) return GraphicsError(evidence, "native-vulkan-surface-bgra8-unavailable", VK_ERROR_FORMAT_NOT_SUPPORTED);
  if ((result = vkGetPhysicalDeviceSurfacePresentModesKHR(vulkan->physicalDevice, window_surface, &count, nullptr)) != VK_SUCCESS)
    return GraphicsError(evidence, "native-vulkan-present-modes", result);
  std::vector<VkPresentModeKHR> modes(count);
  if ((result = vkGetPhysicalDeviceSurfacePresentModesKHR(vulkan->physicalDevice, window_surface, &count, modes.data())) != VK_SUCCESS)
    return GraphicsError(evidence, "native-vulkan-present-modes", result);
  swap_mode = VK_PRESENT_MODE_FIFO_KHR;
  const auto has = [&](VkPresentModeKHR mode) { return std::find(modes.begin(), modes.end(), mode) != modes.end(); };
  if (!present_mode.vsync) {
    if (present_mode.allow_tearing && has(VK_PRESENT_MODE_IMMEDIATE_KHR)) swap_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    else if (has(VK_PRESENT_MODE_MAILBOX_KHR)) swap_mode = VK_PRESENT_MODE_MAILBOX_KHR;
  }
  swap_extent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent :
      VkExtent2D{std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width),
                  std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height)};
  VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  info.surface = window_surface;
  info.minImageCount = std::max(present_mode.buffers, caps.minImageCount);
  if (caps.maxImageCount) info.minImageCount = std::min(info.minImageCount, caps.maxImageCount);
  info.imageFormat = VK_FORMAT_B8G8R8A8_UNORM; info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  info.imageExtent = swap_extent; info.imageArrayLayers = 1;
  info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if ((caps.supportedUsageFlags & info.imageUsage) != info.imageUsage)
    return GraphicsError(evidence, "native-vulkan-swapchain-image-usage", VK_ERROR_FORMAT_NOT_SUPPORTED);
  info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; info.preTransform = caps.currentTransform;
  info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; info.presentMode = swap_mode; info.clipped = VK_TRUE;
  if ((result = vkCreateSwapchainKHR(vulkan->vk, &info, nullptr, &swapchain)) != VK_SUCCESS)
    return GraphicsError(evidence, "native-vulkan-create-swapchain", result);
  if ((result = vkGetSwapchainImagesKHR(vulkan->vk, swapchain, &count, nullptr)) != VK_SUCCESS)
    return GraphicsError(evidence, "native-vulkan-swapchain-images", result);
  std::vector<VkImage> images(count);
  if ((result = vkGetSwapchainImagesKHR(vulkan->vk, swapchain, &count, images.data())) != VK_SUCCESS)
    return GraphicsError(evidence, "native-vulkan-swapchain-images", result);
  back_images.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    auto& back = back_images[i]; back.image = images[i];
    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if ((result = vkCreateSemaphore(vulkan->vk, &sem, nullptr, &back.rendered)) != VK_SUCCESS ||
        (result = vkCreateFence(vulkan->vk, &fence, nullptr, &back.presented)) != VK_SUCCESS)
      return GraphicsError(evidence, "native-vulkan-present-sync", result);
  }
  ++swap_generation; recreate_swapchain = false;
  return true;
}
void Backend::State::ReleaseOutput() {
  if (acquire_fence) vkDestroyFence(vulkan->vk, acquire_fence, nullptr);
  acquire_fence = VK_NULL_HANDLE;
  stages = {}; effects = {};
  output_pipelines.clear();
  for (auto& [name, pipeline] : compute_pipelines) vkDestroyPipeline(vulkan->vk, pipeline, nullptr);
  compute_pipelines.clear();
  for (auto layout : output_layouts) if (layout) vkDestroyPipelineLayout(vulkan->vk, layout, nullptr);
  for (auto set : output_sets) if (set) vkDestroyDescriptorSetLayout(vulkan->vk, set, nullptr);
  output_layouts = {}; output_sets = {};
  if (window_surface) vkDestroySurfaceKHR(static_cast<plume::VulkanInterface*>(render.get())->instance, window_surface, nullptr);
  window_surface = VK_NULL_HANDLE; window = {};
}
// The surface of a native window, by the WSI of its kind. Wayland's types are
// not in Plume's headers (it builds for Xlib on Linux); the instance enables
// VK_KHR_wayland_surface when the loader has it (extend-plume-vulkan.py).
VkResult CreateWindowSurface(VkInstance instance, const platform::NativeWindow& window, VkSurfaceKHR& surface) {
  switch (window.kind) {
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    case platform::NativeWindow::Kind::kWin32: {
      VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
      info.hinstance = static_cast<HINSTANCE>(window.display);
      info.hwnd = static_cast<HWND>(window.window);
      return vkCreateWin32SurfaceKHR(instance, &info, nullptr, &surface);
    }
#endif
#if defined(VK_USE_PLATFORM_XLIB_KHR)
    case platform::NativeWindow::Kind::kXlib: {
      VkXlibSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR};
      info.dpy = static_cast<Display*>(window.display);
      info.window = static_cast<::Window>(reinterpret_cast<uintptr_t>(window.window));
      return vkCreateXlibSurfaceKHR(instance, &info, nullptr, &surface);
    }
#endif
#if defined(__linux__)
    case platform::NativeWindow::Kind::kWayland: {
      const auto create = reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(
          vkGetInstanceProcAddr(instance, "vkCreateWaylandSurfaceKHR"));
      if (!create) return VK_ERROR_EXTENSION_NOT_PRESENT;
      VkWaylandSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR};
      info.display = static_cast<wl_display*>(window.display);
      info.surface = static_cast<wl_surface*>(window.window);
      return create(instance, &info, nullptr, &surface);
    }
#endif
    default:
      return VK_ERROR_EXTENSION_NOT_PRESENT;
  }
}
bool Backend::AttachWindow(const platform::NativeWindow& window, std::string& evidence, const PresentMode& mode) {
  auto& s = *state_;
  uint32_t width = 0, height = 0;
  const bool sized = platform::ClientSize(window, width, height);
  if (!s.ready || s.owner_thread != rex::thread::current_thread_system_id() || s.Attached() ||
      window.kind == platform::NativeWindow::Kind::kNone)
    return GraphicsError(evidence, "native-attach-owner-or-window");
  VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
  VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &maintenance};
  vkGetPhysicalDeviceFeatures2(s.vulkan->physicalDevice, &features);
  if (!maintenance.swapchainMaintenance1)
    return GraphicsError(evidence, "native-vulkan-swapchain-maintenance1-required", VK_ERROR_FEATURE_NOT_PRESENT);
  VkResult result = CreateWindowSurface(static_cast<plume::VulkanInterface*>(s.render.get())->instance, window,
                                        s.window_surface);
  if (result != VK_SUCCESS) return GraphicsError(evidence, "native-vulkan-window-surface", result);
  VkBool32 supported = VK_FALSE;
  result = vkGetPhysicalDeviceSurfaceSupportKHR(s.vulkan->physicalDevice, s.vulkan->queueFamilyIndices[0], s.window_surface, &supported);
  if (result != VK_SUCCESS || !supported) return GraphicsError(evidence, "native-vulkan-window-present-support", result);
  s.window = window; s.present_mode = mode;
  if (sized && !s.CreateSwapchain(width, height, evidence)) return false;
  if (!s.EnsureOutput(evidence)) return false;
  evidence = fmt::format("{{\"attached\":true,\"backend\":\"vulkan\",\"retirement\":\"swapchain_maintenance1 (KHR/EXT)\","
      "\"present_mode\":{},\"buffers\":{},\"effective_frame_latency\":1,\"generation\":{}}}",
      int(s.swap_mode), s.back_images.size(), s.swap_generation);
  return true;
}
bool Backend::DetachWindow(std::string& evidence) {
  auto& s = *state_;
  if (!s.ready || s.owner_thread != rex::thread::current_thread_system_id()) return GraphicsError(evidence, "native-detach-owner");
  if (!WaitIdle(evidence) || !s.RetireSwapchain(evidence)) return false;
  s.ReleaseOutput();
  evidence = "{\"detached\":true,\"presentation_retired\":true}";
  return true;
}
bool Backend::SetGammaRamp(const std::array<uint16_t, 768>& words, std::string& evidence) {
  if (!state_->ready || state_->owner_thread != rex::thread::current_thread_system_id()) return GraphicsError(evidence, "native-gamma-owner");
  for (size_t i = 0; i < words.size(); ++i) state_->gamma[i] = words[i] >> 6;
  evidence = "{\"gamma\":true}"; return true;
}
bool Backend::SetVsync(bool enabled, std::string& evidence) {
  if (!state_->ready || state_->owner_thread != rex::thread::current_thread_system_id()) return GraphicsError(evidence, "native-vsync-owner");
  if (state_->present_mode.vsync != enabled) state_->recreate_swapchain = true;
  state_->present_mode.vsync = enabled; evidence = fmt::format("{{\"vsync\":{}}}", enabled); return true;
}
bool Backend::StageFront(uint32_t key, uint32_t& slot, std::string& evidence) {
  slot = UINT32_MAX; auto& s = *state_;
  if (!s.ready || s.owner_thread != rex::thread::current_thread_system_id() || !s.Attached()) return GraphicsError(evidence, "native-stage-owner-or-window");
  VkImage image{}; SyncState* sync = nullptr; uint64_t* last = nullptr; uint32_t width = 0, height = 0;
  if (auto it = s.textures.find(key); it != s.textures.end() && it->second.texture) {
    auto& t = it->second;
    if (t.depth || !t.content_defined) return GraphicsError(evidence, "native-present-content-undefined");
    image = t.Image(); sync = &t.state; last = &t.last_use; width = t.width; height = t.height;
  } else if (auto it = s.surfaces.find(key); it != s.surfaces.end() &&
      it->second.desc.samples == 1 && it->second.desc.format == SurfaceFormat::Color8888) {
    auto& t = it->second;
    image = t.Image(); sync = &t.state; last = &t.last_use; width = t.desc.width; height = t.desc.height;
  } else return GraphicsError(evidence, "native-stage-source-unknown");
  auto& stage = s.stages[s.staged_frames % s.stages.size()];
  if (stage.staged && !stage.presented) return GraphicsError(evidence, "native-stage-slot-not-presented");
  const char* reason = nullptr; VkResult result = VK_SUCCESS;
  if (!s.OpenBatch(reason, result)) return GraphicsError(evidence, reason, result);
  if (!stage.image || stage.width != width || stage.height != height) {
    if (stage.image) s.batch.retained.push_back(stage.image);
    stage = {};
    if ((result = CreateImage(s.vulkan->allocator, VK_FORMAT_R8G8B8A8_UNORM, width, height, 1, false, stage.image)) != VK_SUCCESS)
      return GraphicsError(evidence, "native-stage-image", result);
    stage.width = width; stage.height = height;
  }
  s.batch.list->setFramebuffer(nullptr);
  auto commands = s.Commands();
  Transition(commands, image, VK_IMAGE_ASPECT_COLOR_BIT, *sync, kCopySource);
  Transition(commands, stage.image->image, VK_IMAGE_ASPECT_COLOR_BIT, stage.state, kCopyDest, true);
  std::vector<VkImageCopy> copies;
  for (const GapCopy& g : PresentCopies(s.render_mode.scale, width, height)) {
    VkImageCopy& copy = copies.emplace_back();
    copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.srcOffset = {int32_t(g.source.left), int32_t(g.source.top), 0};
    copy.dstOffset = {int32_t(g.dest_x), int32_t(g.dest_y), 0};
    copy.extent = {g.source.right - g.source.left, g.source.bottom - g.source.top, 1};
  }
  vkCmdCopyImage(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stage.image->image,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()), copies.data());
  *last = stage.last_use = s.batch.sequence; stage.staged = true; stage.presented = false;
  slot = uint32_t(s.staged_frames++ % s.stages.size()); ++s.batch.operations;
  evidence = fmt::format("{{\"staged\":true,\"slot\":{},\"key\":{},\"size\":[{},{}]}}", slot, key, width, height);
  return true;
}

bool Backend::State::EnsureOutput(std::string& evidence) {
  if (output_layouts[2]) return true;
  // 0: gamma/present; 1: SDK graphics effects; 2: FXAA compute.
  for (unsigned kind = 0; kind < 3; ++kind) {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    if (kind == 0) bindings = {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                               {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    if (kind == 1) bindings = {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                               {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    if (kind == 2) bindings = {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set.bindingCount = uint32_t(bindings.size()); set.pBindings = bindings.data();
    VkResult result = vkCreateDescriptorSetLayout(vulkan->vk, &set, nullptr, &output_sets[kind]);
    if (result != VK_SUCCESS) return GraphicsError(evidence, "native-output-set-layout", result);
    std::vector<VkPushConstantRange> push;
    if (kind == 0) push = {{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32}};
    if (kind == 1) push = {{VK_SHADER_STAGE_VERTEX_BIT, 0, 16}, {VK_SHADER_STAGE_FRAGMENT_BIT, 16, 32}};
    if (kind == 2) push = {{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16}};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1; layout.pSetLayouts = &output_sets[kind];
    layout.pushConstantRangeCount = uint32_t(push.size()); layout.pPushConstantRanges = push.data();
    if ((result = vkCreatePipelineLayout(vulkan->vk, &layout, nullptr, &output_layouts[kind])) != VK_SUCCESS)
      return GraphicsError(evidence, "native-output-layout", result);
  }
  return true;
}
bool Backend::State::OutputTarget(OutputImage& target, uint32_t width, uint32_t height, VkFormat format,
                                   bool storage, std::string& evidence) {
  if (target.image && target.width == width && target.height == height && target.format == format) return true;
  if (target.image) batch.retained.push_back(target.image);
  target = {}; target.width = width; target.height = height; target.format = format;
  const auto result = CreateImage(vulkan->allocator, format, width, height, 1, false, target.image,
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0));
  if (result != VK_SUCCESS) return GraphicsError(evidence, "native-output-target", result);
  return true;
}

bool Backend::State::OutputPass(OutputImage& input, VkImage output, SyncState& output_state, VkFormat format,
    uint32_t width, uint32_t height, const char* shader, unsigned kind, const std::array<uint32_t, 12>& constants,
    VkBuffer ramp, VkDeviceSize ramp_offset, std::string& evidence) {
  auto objects = std::make_shared<GraphicsResources>(); objects->device = vulkan->vk;
  batch.retained.push_back(objects); batch.retained.push_back(input.image);
  VkImageView source{}, target{};
  VkResult result = objects->View(input.image->image, input.format, VK_IMAGE_ASPECT_COLOR_BIT, 1, false, kIdentityMapping, source);
  if (result == VK_SUCCESS) result = objects->View(output, format, VK_IMAGE_ASPECT_COLOR_BIT, 1, false, kIdentityMapping, target);
  if (result != VK_SUCCESS) return GraphicsError(evidence, "native-output-view", result);
  VkSampler sampler{};
  if (kind != 0) {
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.minFilter = info.magFilter = VK_FILTER_LINEAR; info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxLod = VK_LOD_CLAMP_NONE;
    if ((result = vkCreateSampler(vulkan->vk, &info, nullptr, &sampler)) != VK_SUCCESS)
      return GraphicsError(evidence, "native-output-sampler", result);
    objects->samplers.push_back(sampler);
  }
  std::vector<VkDescriptorPoolSize> sizes{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1},
      {kind == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_SAMPLER, 1}};
  if (kind == 2) sizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1});
  VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool.maxSets = 1; pool.poolSizeCount = uint32_t(sizes.size()); pool.pPoolSizes = sizes.data();
  if ((result = vkCreateDescriptorPool(vulkan->vk, &pool, nullptr, &objects->pool)) != VK_SUCCESS)
    return GraphicsError(evidence, "native-output-pool", result);
  VkDescriptorSet set{};
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = objects->pool; alloc.descriptorSetCount = 1; alloc.pSetLayouts = &output_sets[kind];
  if ((result = vkAllocateDescriptorSets(vulkan->vk, &alloc, &set)) != VK_SUCCESS)
    return GraphicsError(evidence, "native-output-set", result);
  VkDescriptorImageInfo image{VK_NULL_HANDLE, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkDescriptorImageInfo sampler_info{sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
  VkDescriptorImageInfo storage{VK_NULL_HANDLE, target, VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorBufferInfo uniform{ramp, ramp_offset, 4096};
  std::vector<VkWriteDescriptorSet> writes;
  auto write = [&](uint32_t binding, VkDescriptorType type, const VkDescriptorImageInfo* image,
                   const VkDescriptorBufferInfo* buffer = nullptr) {
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set; w.dstBinding = binding; w.descriptorCount = 1; w.descriptorType = type;
    w.pImageInfo = image; w.pBufferInfo = buffer; writes.push_back(w);
  };
  write(kind == 2 ? 1 : 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &image);
  if (kind == 0) write(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uniform);
  else write(kind == 2 ? 2 : 1, VK_DESCRIPTOR_TYPE_SAMPLER, &sampler_info);
  if (kind == 2) write(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &storage);
  vkUpdateDescriptorSets(vulkan->vk, uint32_t(writes.size()), writes.data(), 0, nullptr);
  batch.list->setFramebuffer(nullptr);
  const auto commands = Commands();
  const Use read{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VkPipelineStageFlags(kind == 2 ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT), VK_ACCESS_SHADER_READ_BIT};
  Transition(commands, input.image->image, VK_IMAGE_ASPECT_COLOR_BIT, input.state, read);
  if (kind == 2) {
    auto& pipeline = compute_pipelines[shader];
    if (!pipeline) {
      const auto module = PassShader(shader, ShaderStage::kPixel);
      if (!module.Present()) return GraphicsError(evidence, "native-output-compute-module");
      VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      module_info.codeSize = module.bytes->size(); module_info.pCode = reinterpret_cast<const uint32_t*>(module.bytes->data());
      VkShaderModule made{};
      if ((result = vkCreateShaderModule(vulkan->vk, &module_info, nullptr, &made)) != VK_SUCCESS)
        return GraphicsError(evidence, "native-output-compute-module", result);
      VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
      info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
      info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; info.stage.module = made; info.stage.pName = "main";
      info.layout = output_layouts[2];
      result = vkCreateComputePipelines(vulkan->vk, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
      vkDestroyShaderModule(vulkan->vk, made, nullptr);
      if (result != VK_SUCCESS) return GraphicsError(evidence, "native-output-compute-pipeline", result);
    }
    const Use store{VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT};
    Transition(commands, output, VK_IMAGE_ASPECT_COLOR_BIT, output_state, store);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, output_layouts[2], 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(commands, output_layouts[2], VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, constants.data());
    vkCmdDispatch(commands, (width + 15) / 16, (height + 7) / 8, 1);
  } else {
    auto& pipeline = output_pipelines[{shader, format}];
    if (!pipeline) {
      PipelineDesc desc;
      desc.vs = PassShader(kind == 1 ? "guest_output_triangle_strip_rect_vs" : "present_vs", ShaderStage::kVertex);
      desc.ps = PassShader(shader, ShaderStage::kPixel);
      desc.state[0] = 0x00010001; desc.state[1] = 15; desc.state[8] = 1; desc.state[18] = 1;
      // SDK rectangle uses a strip; the gamma shader uses a full-screen triangle.
      desc.state[10] = kind == 1 ? 1 : 0;
      const auto made = MakeGraphicsPipeline(vulkan->vk, output_layouts[kind], desc, {}, pipeline, format, true);
      if (!made.ok) return GraphicsError(evidence, made.reason, VkResult(made.code));
    }
    batch.retained.push_back(pipeline);
    auto* p = static_cast<GraphicsPipeline*>(pipeline.get());
    if ((result = objects->Framebuffer(p->pass, {target}, width, height)) != VK_SUCCESS)
      return GraphicsError(evidence, "native-output-framebuffer", result);
    Transition(commands, output, VK_IMAGE_ASPECT_COLOR_BIT, output_state, kColorTarget);
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = p->pass; begin.framebuffer = objects->framebuffer; begin.renderArea.extent = {width, height};
    vkCmdBeginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{0, 0, float(width), float(height), 0, 1};
    VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdSetViewport(commands, 0, 1, &viewport); vkCmdSetScissor(commands, 0, 1, &scissor);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, output_layouts[kind], 0, 1, &set, 0, nullptr);
    if (kind == 1) {
      vkCmdPushConstants(commands, output_layouts[1], VK_SHADER_STAGE_VERTEX_BIT, 0, 16, constants.data());
      vkCmdPushConstants(commands, output_layouts[1], VK_SHADER_STAGE_FRAGMENT_BIT, 16, 32, constants.data() + 4);
    } else vkCmdPushConstants(commands, output_layouts[0], VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, constants.data());
    vkCmdDraw(commands, kind == 1 ? 4 : 3, 1, 0, 0);
    vkCmdEndRenderPass(commands);
  }
  input.last_use = batch.sequence;
  return true;
}
