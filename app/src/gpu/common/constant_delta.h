#pragma once
#include "gpu/common/draw_storage.h"
#include <bit>
#include <cstring>
#include <span>

namespace gpu {
// Wire format consumed by both backends: uint16 first float4, uint16 count,
// then count * 16 bytes. The mask uses bit 63 for the first 64-byte group.
inline void PackConstantDelta(std::span<const uint8_t, 4096> source, uint64_t pending, ConstantBytes& delta) {
  const uint64_t runs = pending & ~(pending >> 1);
  delta.resize(size_t(std::popcount(pending)) * 64 + size_t(std::popcount(runs)) * 4);
  size_t at = 0;
  unsigned group = 0;
  while (group < 64) {
    if (!((pending >> (63 - group)) & 1)) { ++group; continue; }
    unsigned end = group + 1;
    while (end < 64 && ((pending >> (63 - end)) & 1)) ++end;
    const uint16_t first = uint16_t(4 * group), count = uint16_t(4 * (end - group));
    std::memcpy(delta.data() + at, &first, 2);
    std::memcpy(delta.data() + at + 2, &count, 2);
    std::memcpy(delta.data() + at + 4, source.data() + 64 * group, size_t(count) * 16);
    at += 4 + size_t(count) * 16;
    group = end;
  }
}
}  // namespace gpu
