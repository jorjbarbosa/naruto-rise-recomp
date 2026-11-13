#pragma once

// XMA input packets built from the format, for the native audio's tests
// (audio_xma_stream_test, audio_xma_context_test): frames of given lengths laid
// out in 2048-byte packets as the title's streams have them.
#include "audio/xma/stream.h"

#include <cstdint>
#include <vector>

namespace narutorise::audio::xma::test {
constexpr uint32_t kPayloadBits = kPacketBits - kPacketHeaderBits;

struct Spec {
  uint32_t length;      // Bits, including the 15-bit length.
  bool padding = false;  // The rest of the packet is padding after this frame.
};

struct Built {
  std::vector<uint8_t> bytes;                 // The packets.
  std::vector<std::vector<uint8_t>> frames;   // Each frame's bits, one per byte (0/1).
  std::vector<uint32_t> starts;               // Each frame's start, as a buffer bit offset.
};

inline void SetBit(std::vector<uint8_t>& bytes, uint64_t at, bool value) {
  if (value) bytes[at >> 3] |= uint8_t(0x80 >> (at & 7));
}
inline uint64_t PacketBit(uint64_t payload_bit) {
  return (payload_bit / kPayloadBits) * kPacketBits + kPacketHeaderBits + payload_bit % kPayloadBits;
}

inline Built Build(uint32_t packets, const std::vector<Spec>& specs, uint32_t seed = 7) {
  Built built;
  built.bytes.assign(size_t(packets) * kPacketBytes, 0);
  std::vector<int> first(packets, -1), count(packets, 0);
  uint64_t cursor = 0;
  uint32_t state = seed;
  for (size_t f = 0; f < specs.size(); ++f) {
    const Spec& spec = specs[f];
    const uint64_t start = cursor, end = start + spec.length;
    const uint32_t packet = uint32_t(start / kPayloadBits);
    if (first[packet] < 0) first[packet] = int(start % kPayloadBits);
    ++count[packet];
    // As the title's streams have it: 1 only when the next frame follows in
    // the packet where this one ends and began; a frame that runs into a later
    // packet ends with 0.
    const bool crossed = (end - 1) / kPayloadBits != start / kPayloadBits;
    const bool more = !spec.padding && f + 1 < specs.size() && !crossed && end % kPayloadBits != 0;
    std::vector<uint8_t> bits(spec.length);
    for (uint32_t i = 0; i < spec.length; ++i) {
      bool bit;
      if (i < kFrameLengthBits) {
        bit = (spec.length >> (kFrameLengthBits - 1 - i)) & 1;
      } else if (i == spec.length - 1) {
        bit = more;
      } else {
        state = state * 1103515245 + 12345;
        bit = (state >> 16) & 1;
      }
      bits[i] = bit;
      SetBit(built.bytes, PacketBit(start + i), bit);
    }
    built.frames.push_back(bits);
    built.starts.push_back(uint32_t(PacketBit(start)));
    cursor = spec.padding ? (end + kPayloadBits - 1) / kPayloadBits * kPayloadBits : end;
  }
  for (uint32_t p = 0; p < packets; ++p) {
    uint8_t* header = built.bytes.data() + size_t(p) * kPacketBytes;
    const uint32_t offset = first[p] < 0 ? 0x7FFF : uint32_t(first[p]);
    header[0] = uint8_t((count[p] & 0x3F) << 2 | (offset >> 13));
    header[1] = uint8_t(offset >> 5);
    header[2] = uint8_t((offset & 0x1F) << 3 | 1);  // Metadata 1: XMA2.
    header[3] = 0;
  }
  return built;
}

}  // namespace narutorise::audio::xma::test
