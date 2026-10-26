#pragma once

#include <array>
#include <bit>
#include <cstdint>

namespace gpu::vulkan {
// One descriptor slot is used once per batch and only reused after the fence.
// Image descriptors all have SHADER_READ_ONLY_OPTIMAL layout. Element zero of
// each image set is the backend's permanent fallback (or a null descriptor).
// Samplers belong to the cache and survive every recycle. Views do not.
struct DescriptorWriteState {
  std::array<uint32_t, 4> Plan(const std::array<uint64_t, 128>& key) {
    std::array<uint32_t, 4> masks{};
    for (unsigned set = 0; set < 3; ++set) {
      const uint64_t fallback = key[set * 32];
      uint32_t active = 0;
      for (unsigned i = 0; i < 32; ++i)
        if (key[set * 32 + i] != fallback) active |= uint32_t(1) << i;
      // Every old active entry was invalidated when its view was destroyed.
      // Rewrite it even if a new view gets the SAME numerical handle. Entries
      // no longer active are reset to fallback; new active entries are written.
      masks[set] = initialized && fallbacks[set] == fallback ? active | active_images[set] : UINT32_MAX;
      active_images[set] = active;
      fallbacks[set] = fallback;
    }
    for (unsigned i = 0; i < 32; ++i) {
      if (!initialized || samplers[i] != key[96 + i]) masks[3] |= uint32_t(1) << i;
      samplers[i] = key[96 + i];
    }
    initialized = true;
    return masks;
  }
 private:
  bool initialized = false;
  std::array<uint32_t, 3> active_images{};
  std::array<uint64_t, 3> fallbacks{};
  std::array<uint64_t, 32> samplers{};
};

template <typename F> void DescriptorRanges(uint32_t mask, F&& emit) {
  while (mask) {
    const unsigned first = std::countr_zero(mask);
    const unsigned count = std::countr_one(mask >> first);
    emit(first, count);
    // Handle a full 32-element range without a shift by the word size.
    const uint32_t run = count == 32 ? UINT32_MAX : ((uint32_t(1) << count) - 1) << first;
    mask &= ~run;
  }
}
}  // namespace gpu::vulkan
