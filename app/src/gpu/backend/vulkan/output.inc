bool Backend::State::OutputEffects(OutputImage& source, BackImage& back, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height, VkBuffer ramp, VkDeviceSize ramp_offset, std::string& evidence) {
  const auto bits = [](float f) { return std::bit_cast<uint32_t>(f); };
  const bool fxaa = present_mode.post_effect != SwapPostEffect::kNone;
  auto& ramped = effects[0];
  if (!OutputTarget(ramped, source.width, source.height, fxaa ? VK_FORMAT_R16G16B16A16_UNORM :
      VK_FORMAT_A2B10G10R10_UNORM_PACK32, false, evidence)) return false;
  if (!OutputPass(source, ramped.image->image, ramped.state, ramped.format, ramped.width, ramped.height,
      fxaa ? "present_gamma_luma_ps" : "present_gamma_ps", 0, {}, ramp, ramp_offset, evidence)) return false;
  OutputImage* input = &ramped;
  if (fxaa) {
    auto& target = effects[1];
    if (!OutputTarget(target, source.width, source.height, VK_FORMAT_A2B10G10R10_UNORM_PACK32, true, evidence)) return false;
    std::array<uint32_t, 12> c{source.width, source.height, bits(1.0f / source.width), bits(1.0f / source.height)};
    if (!OutputPass(*input, target.image->image, target.state, target.format, target.width, target.height,
        present_mode.post_effect == SwapPostEffect::kFxaaExtreme ? "fxaa_extreme_cs" : "fxaa_cs",
        2, c, VK_NULL_HANDLE, 0, evidence)) return false;
    input = &target;
  }
  const auto plan = OutputPlan(present_mode, source.width, source.height, width, height);
  if (plan.size() + 1 > effects.size()) return GraphicsError(evidence, "native-output-plan-too-long");
  Transition(Commands(), back.image, VK_IMAGE_ASPECT_COLOR_BIT, back.state, kCopyDest);
  const VkClearColorValue black{{0, 0, 0, 1}};
  const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdClearColorImage(Commands(), back.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
  for (size_t i = 0; i < plan.size(); ++i) {
    const auto& step = plan[i]; const bool last = i + 1 == plan.size();
    VkImage output = back.image; SyncState* sync = &back.state; VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    uint32_t dw = swap_extent.width, dh = swap_extent.height, ox = x, oy = y;
    if (!last) {
      auto& target = effects[2 + i];
      if (!OutputTarget(target, step.width, step.height, VK_FORMAT_A2B10G10R10_UNORM_PACK32, false, evidence)) return false;
      output = target.image->image; sync = &target.state; format = target.format; dw = target.width; dh = target.height;
      ox = oy = 0;
    }
    // The SDK SPIR-V rectangle is already in Vulkan clip coordinates.
    std::array<uint32_t, 12> c{bits(-1.0f + 2.0f * ox / dw), bits(-1.0f + 2.0f * oy / dh),
        bits(2.0f * step.width / dw), bits(2.0f * step.height / dh)};
    auto* p = c.data() + 4;
    const char* shader = "guest_output_bilinear_ps";
    switch (step.pass) {
      case gpu::OutputPass::kBilinear:
        p[0] = ox; p[1] = oy; p[2] = bits(1.0f / step.width); p[3] = bits(1.0f / step.height); break;
      case gpu::OutputPass::kCasSharpen:
        shader = "guest_output_ffx_cas_sharpen_ps";
        p[0] = ox; p[1] = oy; p[2] = bits(-1.0f / (8.0f - 3.0f * present_mode.cas_sharpness)); break;
      case gpu::OutputPass::kCasResample:
        shader = "guest_output_ffx_cas_resample_ps";
        p[0] = ox; p[1] = oy; p[2] = bits(float(input->width) / step.width); p[3] = bits(float(input->height) / step.height);
        p[4] = bits(-1.0f / (8.0f - 3.0f * present_mode.cas_sharpness)); break;
      case gpu::OutputPass::kFsrEasu:
        shader = "guest_output_ffx_fsr_easu_ps";
        p[0] = bits(float(input->width) / step.width); p[1] = bits(float(input->height) / step.height);
        p[2] = bits(1.0f / input->width); p[3] = bits(1.0f / input->height); break;
      case gpu::OutputPass::kFsrRcas:
        shader = "guest_output_ffx_fsr_rcas_ps";
        p[0] = ox; p[1] = oy; p[2] = bits(std::exp2(-present_mode.fsr_sharpness_reduction)); break;
    }
    if (!OutputPass(*input, output, *sync, format, dw, dh, shader, 1, c, VK_NULL_HANDLE, 0, evidence)) return false;
    if (!last) input = &effects[2 + i];
  }
  return true;
}

bool Backend::Present(uint32_t slot, bool capture, std::vector<uint8_t>& bgra, uint32_t& width,
                       uint32_t& height, std::string& evidence, bool& presented) {
  presented = false;
  auto& s = *state_; bgra.clear(); width = height = 0;
  if (!s.ready || s.owner_thread != GetCurrentThreadId() || !s.window || slot >= s.stages.size())
    return GraphicsError(evidence, "native-present-owner-or-slot");
  auto& stage = s.stages[slot];
  if (!stage.staged || stage.presented) return GraphicsError(evidence, "native-present-slot-not-staged");
  RECT rect{};
  if (!IsWindow(s.window) || !GetClientRect(s.window, &rect) || IsIconic(s.window) || rect.right <= 0 || rect.bottom <= 0) {
    stage.presented = true; evidence = "{\"presented\":false,\"skipped\":\"window-empty\"}"; return true;
  }
  const char* reason = nullptr; VkResult result = VK_SUCCESS;
  if (!s.CloseBatch(reason, result)) return GraphicsError(evidence, reason, result);
  if (!s.swapchain || s.recreate_swapchain || uint32_t(rect.right) != s.swap_extent.width || uint32_t(rect.bottom) != s.swap_extent.height)
    if (!s.CreateSwapchain(rect.right, rect.bottom, evidence)) return false;
  // Bound the presentation queue to one pending frame independently of render completion.
  for (auto& back : s.back_images) if (back.pending) {
    if (!s.WaitPresent(back.presented, evidence)) return false;
    back.pending = false;
  }
  if (!s.acquire_fence) {
    VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if ((result = vkCreateFence(s.vulkan->vk, &info, nullptr, &s.acquire_fence)) != VK_SUCCESS)
      return GraphicsError(evidence, "native-vulkan-acquire-fence", result);
  }
  if ((result = vkResetFences(s.vulkan->vk, 1, &s.acquire_fence)) != VK_SUCCESS)
    return GraphicsError(evidence, "native-vulkan-acquire-reset", result);
  uint32_t index = 0;
  const auto deadline = std::chrono::steady_clock::now() + kWaitBudget;
  std::vector<int32_t> acquire_results;
  do {
    if (!IsWindow(s.window) || IsIconic(s.window)) {
      stage.presented = true; evidence = "{\"presented\":false,\"skipped\":\"window-closed-or-minimized\"}"; return true;
    }
    result = vkAcquireNextImageKHR(s.vulkan->vk, s.swapchain, 20'000'000, VK_NULL_HANDLE, s.acquire_fence, &index);
    acquire_results.push_back(int32_t(result));
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
      GetClientRect(s.window, &rect);
      if (rect.right <= 0 || rect.bottom <= 0) {
        stage.presented = true; evidence = "{\"presented\":false,\"skipped\":\"window-empty\"}"; return true;
      }
      if (!s.CreateSwapchain(rect.right, rect.bottom, evidence)) return false;
    } else if (result != VK_TIMEOUT && result != VK_NOT_READY) break;
  } while (std::chrono::steady_clock::now() < deadline && !s.failures->Failed());
  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    s.Observe(result); return GraphicsError(evidence, "native-vulkan-acquire", result);
  }
  if (result == VK_SUBOPTIMAL_KHR) s.recreate_swapchain = true;
  s.acquire_pending = true;
  if (!s.WaitPresent(s.acquire_fence, evidence)) return false;
  s.acquire_pending = false;
  auto& back = s.back_images.at(index);
  if (!s.OpenBatch(reason, result)) return GraphicsError(evidence, reason, result);
  const bool ui = s.UiFrameVisible();
  if (ui && !s.EnsureUiLayer(evidence)) return false;
  State::UploadPage* page{}; VkDeviceSize ramp_offset{};
  if (!s.Upload(4096, page, ramp_offset, result)) return GraphicsError(evidence, "native-present-ramp-upload", result);
  auto* ramp = reinterpret_cast<uint32_t*>(page->buffer->mapped + ramp_offset);
  const bool brightness = std::fabs(s.present_mode.brightness - 0.5f) > 0.001f;
  const double exponent = 1.0 / std::clamp(1.0 + (double(s.present_mode.brightness) - 0.5) * 1.2, 0.1, 4.0);
  for (uint32_t i = 0; i < 256; ++i) for (uint32_t channel = 0; channel < 4; ++channel) {
    const uint32_t value = channel == 3 ? 0 : s.gamma[channel * 256 + i];
    ramp[4 * i + channel] = brightness ? uint32_t(std::lround(1023.0 * std::pow(std::min(value, 1023u) / 1023.0, exponent))) : value;
  }
  const double aspect_scale = std::min(double(s.swap_extent.width) / stage.width, double(s.swap_extent.height) / stage.height);
  const uint32_t vw = std::max(1u, uint32_t(std::lround(stage.width * aspect_scale)));
  const uint32_t vh = std::max(1u, uint32_t(std::lround(stage.height * aspect_scale)));
  const uint32_t x = (s.swap_extent.width - vw) / 2, y = (s.swap_extent.height - vh) / 2;
  const auto bits = [](float f) { return std::bit_cast<uint32_t>(f); };
  if (s.present_mode.effect != PresentEffect::kBilinear || s.present_mode.post_effect != SwapPostEffect::kNone) {
    if (!s.OutputEffects(stage, back, x, y, vw, vh, page->buffer->buffer, ramp_offset, evidence)) return false;
  } else {
    const std::array<uint32_t, 12> c{bits(float(x)), bits(float(y)), bits(float(stage.width) / vw), bits(float(stage.height) / vh),
        stage.width, stage.height, bits(float(vw)), bits(float(vh))};
    if (!s.OutputPass(stage, back.image, back.state, VK_FORMAT_B8G8R8A8_UNORM, s.swap_extent.width, s.swap_extent.height,
                      "present_ps", 0, c, page->buffer->buffer, ramp_offset, evidence)) return false;
  }
  // The UI layer over the image and its bars.
  if (ui && !s.DrawUi(back, evidence)) return false;
  std::shared_ptr<VmaBuffer> readback;
  const VkDeviceSize bytes = VkDeviceSize(s.swap_extent.width) * s.swap_extent.height * 4;
  if (capture) {
    result = CreateBuffer(s.vulkan->allocator, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT, readback);
    if (result != VK_SUCCESS) return GraphicsError(evidence, "native-present-readback", result);
    s.batch.retained.push_back(readback);
    Transition(s.Commands(), back.image, VK_IMAGE_ASPECT_COLOR_BIT, back.state, kCopySource);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {s.swap_extent.width, s.swap_extent.height, 1};
    vkCmdCopyImageToBuffer(s.Commands(), back.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->buffer, 1, &copy);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(s.Commands(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
  }
  const Use present{VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0};
  Transition(s.Commands(), back.image, VK_IMAGE_ASPECT_COLOR_BIT, back.state, present);
  stage.last_use = s.batch.sequence; const uint64_t sequence = s.batch.sequence;
  s.submit_signal = back.rendered;
  if (!s.CloseBatch(reason, result)) return GraphicsError(evidence, reason, result);
  if ((result = vkResetFences(s.vulkan->vk, 1, &back.presented)) != VK_SUCCESS)
    return GraphicsError(evidence, "native-present-fence-reset", result);
  VkSwapchainPresentFenceInfoEXT completion{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
  completion.swapchainCount = 1; completion.pFences = &back.presented;
  VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, &completion};
  info.waitSemaphoreCount = 1; info.pWaitSemaphores = &back.rendered;
  info.swapchainCount = 1; info.pSwapchains = &s.swapchain; info.pImageIndices = &index;
  auto* queue = static_cast<plume::VulkanCommandQueue*>(s.queue.get())->queue;
  const bool authorized = s.failures->Authorize([&] {
    std::lock_guard lock(*queue->mutex);
    result = vkQueuePresentKHR(queue->vk, &info);
  });
  if (!authorized) return GraphicsError(evidence, "native-present-refused-after-failure");
  back.pending = true;
  stage.presented = true; ++s.presents;
  const VkResult present_result = result;
  if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) s.recreate_swapchain = true;
  else if (result != VK_SUCCESS) {
    s.Observe(result); return GraphicsError(evidence, "native-vulkan-present", result);
  }
  if (capture) {
    if (!s.completion->WaitFor(sequence, kWaitBudget)) return GraphicsError(evidence, "native-present-capture-wait", VK_TIMEOUT);
    if ((result = vmaInvalidateAllocation(s.vulkan->allocator, readback->allocation, 0, bytes)) != VK_SUCCESS)
      return GraphicsError(evidence, "native-present-capture-invalidate", result);
    width = s.swap_extent.width; height = s.swap_extent.height;
    bgra.assign(readback->mapped, readback->mapped + bytes);
  }
  presented = present_result == VK_SUCCESS || present_result == VK_SUBOPTIMAL_KHR;
  evidence = fmt::format("{{\"presented\":{},\"backend\":\"vulkan\",\"present_result\":{},\"acquire_results\":[{}],"
      "\"generation\":{},\"image\":{},\"size\":[{},{}],\"fence\":{},\"present_mode\":{},\"effective_frame_latency\":1,"
      "\"ui\":{}}}",
      presented, int32_t(present_result), fmt::join(acquire_results, ","), s.swap_generation, index, s.swap_extent.width,
      s.swap_extent.height, sequence, int(s.swap_mode), ui);
  return true;
}
