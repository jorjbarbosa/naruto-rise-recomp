// Immutable draw bindings shared within one GPU batch. The batch owns this
// cache until its submission completes, before any retired images are freed.
#pragma once
#include "gpu/backend/vulkan/graphics.h"
#include "gpu/backend/vulkan/descriptor_writes.h"
#include <cstring>
#include <unordered_map>

namespace gpu::vulkan {
struct DrawCacheCounters {
  std::atomic<uint64_t> draws{0}, pools{0}, groups{0}, views{0}, samplers{0}, framebuffers{0}, constant_bytes{0}, passes{0};
  std::atomic<uint64_t> pipeline_binds{0}, descriptor_binds{0};
  std::atomic<uint64_t> set_allocations{0};
#if defined(NATIVE_RENDERER_ENGINE_FAULT_TEST)
  // Actual elements sent to Vulkan: a regression oracle, absent from the product.
  std::atomic<uint64_t> descriptor_elements{0};
#endif
};
inline size_t HashWords(const uint64_t* words, size_t count) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < count; ++i) { h ^= words[i]; h *= 0x100000001B3ull; }
  // Handles are commonly aligned pointers. Mix their high bits into low
  // bits too: MSVC's unordered_map uses power-of-two bucket counts.
  h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ull;
  h ^= h >> 27; h *= 0x94D049BB133111EBull;
  return size_t(h ^ (h >> 31));
}
template <size_t N> struct DrawKeyHash {
  size_t operator()(const std::array<uint64_t, N>& key) const { return HashWords(key.data(), N); }
};
class DrawCache {
 public:
  // A draw's textures by register slot: the view in the 2D or the cube image
  // set and the sampler. Only the slots of the masks are read; the others
  // keep the fallback views and the default sampler.
  struct Bindings {
    uint32_t images = 0, cubes = 0, samplers = 0;
    std::array<VkImageView, 32> image, cube;
    std::array<VkSampler, 32> sampler;
    void View(bool is_cube, uint32_t slot, VkImageView view) {
      (is_cube ? cube : image)[slot] = view;
      (is_cube ? cubes : images) |= uint32_t(1) << slot;
    }
    void Sampler(uint32_t slot, VkSampler made) { sampler[slot] = made; samplers |= uint32_t(1) << slot; }
  };
  DrawCache(VkDevice device, DrawCacheCounters& counters) : device_(device), counters_(counters) {}
  DrawCache(const DrawCache&) = delete;
  DrawCache& operator=(const DrawCache&) = delete;
  ~DrawCache() {
    Recycle();
    for (auto pool : pools_) vkDestroyDescriptorPool(device_, pool, nullptr);
    for (const auto& [key, sampler] : samplers_) vkDestroySampler(device_, sampler, nullptr);
  }
  // Called only after the batch's GPU fence completes. Active image slots are
  // rewritten before reuse, including those whose numeric handles repeat.
  // Permanent fallback descriptors and owned samplers survive the recycle.
  void Recycle() {
    for (const auto& [key, framebuffer] : framebuffers_) vkDestroyFramebuffer(device_, framebuffer, nullptr);
    for (const auto& [key, view] : views_) vkDestroyImageView(device_, view, nullptr);
    framebuffers_.clear(); views_.clear(); groups_.clear();
    used_slots_ = 0;
  }
  VkResult View(VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t levels,
                bool cube, const ComponentMapping& mapping, VkImageView& out) {
    uint32_t swizzle = 0;
    for (unsigned i = 0; i < 4; ++i) swizzle |= uint32_t(mapping[i]) << (i * 3);
    const ViewKey key{std::bit_cast<uint64_t>(image), uint64_t(format), aspect, levels, cube, swizzle};
    if (auto it = views_.find(key); it != views_.end()) { out = it->second; return VK_SUCCESS; }
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image; info.format = format;
    info.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    info.components = ImageViewMapping(mapping);
    info.subresourceRange = {aspect, 0, levels, 0, cube ? 6u : 1u};
    const auto result = vkCreateImageView(device_, &info, nullptr, &out);
    if (result == VK_SUCCESS) { views_.emplace(key, out); counters_.views.fetch_add(1, std::memory_order_relaxed); }
    return result;
  }
  VkResult Sampler(const VkSamplerCreateInfo& info, VkSampler& out) {
    // Explicit fields, never structure padding or the address of pNext.
    SamplerKey key{info.flags, uint64_t(info.magFilter), uint64_t(info.minFilter), uint64_t(info.mipmapMode),
        uint64_t(info.addressModeU), uint64_t(info.addressModeV), uint64_t(info.addressModeW),
        std::bit_cast<uint32_t>(info.mipLodBias), info.anisotropyEnable, std::bit_cast<uint32_t>(info.maxAnisotropy),
        info.compareEnable, uint64_t(info.compareOp), std::bit_cast<uint32_t>(info.minLod),
        std::bit_cast<uint32_t>(info.maxLod), uint64_t(info.borderColor), info.unnormalizedCoordinates};
    if (info.pNext) {
      const auto* border = static_cast<const VkSamplerCustomBorderColorCreateInfoEXT*>(info.pNext);
      if (border->sType != VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT || border->pNext)
        return VK_ERROR_FEATURE_NOT_PRESENT;
      key[16] = uint64_t(border->format);
      for (unsigned i = 0; i < 4; ++i) {
        uint32_t bits;
        std::memcpy(&bits, reinterpret_cast<const uint8_t*>(&border->customBorderColor) + i * 4, 4);
        key[17 + i] = bits;
      }
    }
    if (auto it = samplers_.find(key); it != samplers_.end()) { out = it->second; return VK_SUCCESS; }
    const auto result = vkCreateSampler(device_, &info, nullptr, &out);
    if (result == VK_SUCCESS) { samplers_.emplace(key, out); counters_.samplers.fetch_add(1, std::memory_order_relaxed); }
    return result;
  }
  // The sampler of the slots without a texture (point filtering, every LOD),
  // created once; like the other samplers it survives every recycle.
  VkResult DefaultSampler(VkSampler& out) {
    if (!default_sampler_) {
      VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
      info.maxLod = VK_LOD_CLAMP_NONE;
      VkSampler made{};
      if (const auto result = Sampler(info, made); result != VK_SUCCESS) return result;
      default_sampler_ = made;
    }
    out = default_sampler_;
    return VK_SUCCESS;
  }
  VkResult Framebuffer(VkRenderPass pass, const std::vector<VkImageView>& attachments,
                        uint32_t width, uint32_t height, uint32_t compatibility, VkFramebuffer& out) {
    // All draw render passes come from MakeRenderPass: fixed RGBA8/D32S8,
    // matching subpass structure. Only attachment presence and samples vary.
    const FrameKey key{compatibility,
        attachments.empty() ? 0 : std::bit_cast<uint64_t>(attachments[0]),
        attachments.size() < 2 ? 0 : std::bit_cast<uint64_t>(attachments[1]), width, height};
    if (auto it = framebuffers_.find(key); it != framebuffers_.end()) { out = it->second; return VK_SUCCESS; }
    VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    info.renderPass = pass; info.attachmentCount = uint32_t(attachments.size()); info.pAttachments = attachments.data();
    info.width = width; info.height = height; info.layers = 1;
    const auto result = vkCreateFramebuffer(device_, &info, nullptr, &out);
    if (result == VK_SUCCESS) { framebuffers_.emplace(key, out); counters_.framebuffers.fetch_add(1, std::memory_order_relaxed); }
    return result;
  }
  // The descriptor sets of a draw: set 0 the 2D images, 1 the 3D ones (always
  // the fallback), 2 the cubes, 3 the samplers. The fallbacks hold for the
  // cache's life (the backend replaces them only once every cache is gone),
  // so a group is found by the draw's bindings alone; all 128 elements are
  // laid out only for a new group.
  VkResult Descriptors(const std::array<VkDescriptorSetLayout, 4>& layouts, const std::array<VkImageView, 3>& fallbacks,
      VkSampler fallback_sampler, const Bindings& bindings, std::array<VkDescriptorSet, 4>& out) {
    auto& lookup = lookup_;
    lookup.words[0] = bindings.images | uint64_t(bindings.cubes) << 32;
    lookup.words[1] = bindings.samplers;
    lookup.count = 2;
    auto put = [&](uint32_t mask, const auto& handles) {
      for (; mask; mask &= mask - 1) lookup.words[lookup.count++] = std::bit_cast<uint64_t>(handles[std::countr_zero(mask)]);
    };
    put(bindings.images, bindings.image);
    put(bindings.cubes, bindings.cube);
    put(bindings.samplers, bindings.sampler);
    if (auto it = groups_.find(lookup); it != groups_.end()) { out = it->second; return VK_SUCCESS; }
    std::array<std::array<VkDescriptorImageInfo, 32>, 4> images{};
    for (unsigned i = 0; i < 32; ++i) {
      for (unsigned set = 0; set < 3; ++set) {
        images[set][i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        images[set][i].imageView = fallbacks[set];
      }
      images[3][i].sampler = fallback_sampler;
    }
    for (uint32_t mask = bindings.images; mask; mask &= mask - 1)
      images[0][std::countr_zero(mask)].imageView = bindings.image[std::countr_zero(mask)];
    for (uint32_t mask = bindings.cubes; mask; mask &= mask - 1)
      images[2][std::countr_zero(mask)].imageView = bindings.cube[std::countr_zero(mask)];
    for (uint32_t mask = bindings.samplers; mask; mask &= mask - 1)
      images[3][std::countr_zero(mask)].sampler = bindings.sampler[std::countr_zero(mask)];
    GroupKey key;
    for (unsigned set = 0; set < 4; ++set) for (unsigned i = 0; i < 32; ++i)
      key[set * 32 + i] = set == 3 ? std::bit_cast<uint64_t>(images[set][i].sampler) :
                                      std::bit_cast<uint64_t>(images[set][i].imageView);
    if (used_slots_ == slots_.size()) {
      if (pools_.empty() || groups_in_pool_ == kGroupsPerPool) {
        VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 96 * kGroupsPerPool},
                                     {VK_DESCRIPTOR_TYPE_SAMPLER, 32 * kGroupsPerPool}};
        VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.maxSets = 4 * kGroupsPerPool; info.poolSizeCount = 2; info.pPoolSizes = sizes;
        VkDescriptorPool pool{};
        const auto result = vkCreateDescriptorPool(device_, &info, nullptr, &pool);
        if (result != VK_SUCCESS) return result;
        pools_.push_back(pool); groups_in_pool_ = 0;
        counters_.pools.fetch_add(1, std::memory_order_relaxed);
      }
      VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      info.descriptorPool = pools_.back(); info.descriptorSetCount = 4; info.pSetLayouts = layouts.data();
      const auto result = vkAllocateDescriptorSets(device_, &info, out.data());
      if (result != VK_SUCCESS) return result;
      ++groups_in_pool_;
      slots_.push_back({out, {}});
      counters_.set_allocations.fetch_add(4, std::memory_order_relaxed);
    } else {
      out = slots_[used_slots_].sets;
    }
    const auto masks = slots_[used_slots_++].previous.Plan(key);
    // At most sixteen disjoint runs per 32-bit mask, four masks. A new slot is
    // initialized in full; recycled slots update only invalid/changed entries.
    std::array<VkWriteDescriptorSet, 64> writes;
    uint32_t count = 0;
    for (unsigned set = 0; set < 4; ++set) {
      DescriptorRanges(masks[set], [&](unsigned first, unsigned length) {
        auto& write = writes[count++];
        write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = out[set]; write.dstArrayElement = first; write.descriptorCount = length;
        write.descriptorType = set == 3 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        write.pImageInfo = images[set].data() + first;
      });
    }
    if (count) vkUpdateDescriptorSets(device_, count, writes.data(), 0, nullptr);
#if defined(NATIVE_RENDERER_ENGINE_FAULT_TEST)
    for (uint32_t i = 0; i < count; ++i)
      counters_.descriptor_elements.fetch_add(writes[i].descriptorCount, std::memory_order_relaxed);
#endif
    groups_.emplace(lookup, out);
    counters_.groups.fetch_add(1, std::memory_order_relaxed);
    return VK_SUCCESS;
  }
 private:
  static constexpr uint32_t kGroupsPerPool = 64;
  using ViewKey = std::array<uint64_t, 6>;
  using SamplerKey = std::array<uint64_t, 21>;
  using FrameKey = std::array<uint64_t, 5>;
  using GroupKey = std::array<uint64_t, 128>;
  // A group by its bindings: the masks, then the bound handles in slot order.
  // Only the first count words count; the lookup key is reused by every draw.
  struct BindingKey {
    uint32_t count = 0;
    std::array<uint64_t, 2 + 3 * 32> words{};
    bool operator==(const BindingKey& other) const {
      return count == other.count && !std::memcmp(words.data(), other.words.data(), count * sizeof(uint64_t));
    }
  };
  struct BindingKeyHash {
    size_t operator()(const BindingKey& key) const { return HashWords(key.words.data(), key.count); }
  };
  BindingKey lookup_;
  VkSampler default_sampler_ = VK_NULL_HANDLE;
  const VkDevice device_;
  DrawCacheCounters& counters_;
  std::vector<VkDescriptorPool> pools_;
  struct Slot { std::array<VkDescriptorSet, 4> sets; DescriptorWriteState previous; };
  std::vector<Slot> slots_;
  size_t used_slots_ = 0;
  uint32_t groups_in_pool_ = 0;
  std::unordered_map<ViewKey, VkImageView, DrawKeyHash<6>> views_;
  std::unordered_map<SamplerKey, VkSampler, DrawKeyHash<21>> samplers_;
  std::unordered_map<FrameKey, VkFramebuffer, DrawKeyHash<5>> framebuffers_;
  std::unordered_map<BindingKey, std::array<VkDescriptorSet, 4>, BindingKeyHash> groups_;
};
}  // namespace gpu::vulkan
