#include "gpu/backend/vulkan/descriptor_writes.h"
#undef NDEBUG
#include <cassert>
#include <iostream>
#include <random>

using namespace gpu::vulkan;
int main() {
  // Model descriptor storage, and deliberately recycle the same numerical
  // handles for new view generations. Unwritten active entries retain the
  // wrong generation even if their numbers compare equal.
  DescriptorWriteState state;
  std::array<uint64_t, 128> key{}, stored{}, generation{};
  std::mt19937 random(0x1234);
  uint64_t written = 0;
  for (uint64_t batch = 1; batch <= 2000; ++batch) {
    for (unsigned set = 0; set < 3; ++set) {
      // Null and real permanent fallbacks; change the fallback sometimes.
      const uint64_t fallback = (batch / 100) % 2 ? 100 + set : 0;
      for (unsigned i = 0; i < 32; ++i) {
        const bool active = i && (random() % 7 == 0);
        key[set * 32 + i] = active ? 1000 + i : fallback;
      }
    }
    for (unsigned i = 96; i < 128; ++i) key[i] = 1 + (random() % 4);
    const auto masks = state.Plan(key);
    if (batch == 1) for (auto mask : masks) assert(mask == UINT32_MAX);
    for (unsigned set = 0; set < 4; ++set) {
      uint32_t covered = 0; unsigned runs = 0;
      DescriptorRanges(masks[set], [&](unsigned first, unsigned count) {
        assert(count && first + count <= 32); ++runs;
        for (unsigned i = first; i < first + count; ++i) {
          assert(!(covered & (uint32_t(1) << i))); covered |= uint32_t(1) << i;
          const auto at = set * 32 + i;
          stored[at] = key[at]; generation[at] = batch; ++written;
        }
      });
      assert(covered == masks[set] && runs <= 16);
      for (unsigned i = 0; i < 32; ++i) {
        const auto at = set * 32 + i;
        assert(stored[at] == key[at]);
        if (set < 3 && key[at] != key[set * 32]) assert(generation[at] == batch);
      }
    }
  }
  assert(written < 2000 * 128 * 0.6);
  // A completely idle slot needs no second write, including real fallbacks.
  DescriptorWriteState idle;
  key.fill(7); idle.Plan(key);
  for (auto mask : idle.Plan(key)) assert(mask == 0);
  // Explicit edge cases for contiguous-run emission, including bit 31.
  for (uint32_t mask : {0u, UINT32_MAX, 0x80000000u, 0xAAAAAAAAu, 0x55555555u}) {
    uint32_t result = 0;
    DescriptorRanges(mask, [&](unsigned first, unsigned count) {
      for (unsigned i = first; i < first + count; ++i) result |= uint32_t(1) << i;
    });
    assert(result == mask);
  }
  std::cout << "PASS: initialization, stale slots, recycled handles, fallback/sampler changes and ranges\n";
}
