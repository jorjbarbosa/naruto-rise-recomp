// Reading XMA frames out of a context's input packets (stream.h).
#include "audio/xma/stream.h"

#include <algorithm>
#include <cstring>

namespace narutorise::audio::xma {
namespace {
// A packet in the sequence and a bit in it.
struct Place {
  uint32_t buffer = 0;
  uint32_t packet = 0;
  uint32_t bit = 0;
};

uint32_t FirstFrameBit(const uint8_t* packet) {
  return ((uint32_t(packet[0] & 0x3) << 13) | (uint32_t(packet[1]) << 5) | (packet[2] >> 3)) + kPacketHeaderBits;
}
uint32_t SkipCount(const uint8_t* packet) { return packet[3]; }

class Packets {
 public:
  Packets(const InputBuffer (&buffers)[2], uint32_t first) : buffers_(buffers), first_(first) {}
  const uint8_t* At(const Place& place) const {
    const InputBuffer& b = buffers_[place.buffer];
    if (!b.valid || !b.data || place.packet >= b.packets) return nullptr;
    return b.data + size_t(place.packet) * kPacketBytes;
  }
  // The stream's packet after `place`'s, at its payload start. kNeedInput when
  // it is in the other buffer and that is not valid; kEnd past the other one.
  Read Step(Place& place, uint32_t& skipped) const {
    const uint8_t* packet = At(place);
    if (!packet) return Read::kBadData;
    uint32_t skip = SkipCount(packet);
    if (skip == 0xFF) skip = 0;  // No frame begins here; the stream goes on in the next packet.
    skipped += skip;
    uint32_t next = place.packet + 1 + skip;
    uint32_t buffer = place.buffer;
    const uint32_t count = buffers_[buffer].packets;
    if (next >= count) {
      if (buffer != first_) return Read::kEnd;  // Only one buffer ahead is ever looked at.
      next -= count;
      buffer ^= 1;
      if (!buffers_[buffer].valid || !buffers_[buffer].data) return Read::kNeedInput;
      if (next >= buffers_[buffer].packets) return Read::kBadData;
    }
    place = {buffer, next, kPacketHeaderBits};
    return Read::kFrame;
  }

 private:
  const InputBuffer (&buffers_)[2];
  uint32_t first_;
};

// Up to 24 bits at `bit` of `packet`, most significant first.
uint32_t Bits(const uint8_t* packet, uint32_t bit, uint32_t count) {
  const uint32_t first = bit >> 3;
  const uint32_t last = (bit + count - 1) >> 3;
  uint32_t window = 0;
  for (uint32_t i = first; i <= last; ++i) window = (window << 8) | packet[i];
  const uint32_t used = (last - first + 1) * 8;
  return (window >> (used - (bit & 7) - count)) & ((1u << count) - 1);
}

// ORs `count` bits of `value` (most significant first) at bit `at` of `out`.
void Put(uint8_t* out, uint32_t at, uint32_t value, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    if ((value >> (count - 1 - i)) & 1) out[(at + i) >> 3] |= uint8_t(0x80 >> ((at + i) & 7));
  }
}

// Copies `count` bits from `place` through the payloads of the following
// packets, to bit `at` of `out` (when `out` is not null). Leaves `place` just
// after the last bit copied.
Read Copy(const Packets& packets, Place& place, uint32_t count, uint8_t* out, uint32_t at, uint32_t& skipped) {
  while (count) {
    if (place.bit >= kPacketBits) {
      if (const Read step = packets.Step(place, skipped); step != Read::kFrame)
        return step == Read::kEnd ? Read::kNeedInput : step;
    }
    const uint8_t* packet = packets.At(place);
    if (!packet) return Read::kBadData;
    const uint32_t take = std::min(count, kPacketBits - place.bit);
    for (uint32_t done = 0; done < take;) {
      const uint32_t chunk = std::min<uint32_t>(take - done, 24);
      if (out) Put(out, at, Bits(packet, place.bit + done, chunk), chunk);
      at += chunk;
      done += chunk;
    }
    place.bit += take;
    count -= take;
  }
  return Read::kFrame;
}

// Moves `place` to the first frame that begins in the stream's packet after
// it (or in its own packet, when `same` and it begins at or after the place).
Read NextFrameStart(const Packets& packets, Place& place, bool same, uint32_t& skipped) {
  for (int guard = 0; guard < 4096; ++guard) {
    if (same && place.bit < kPacketBits) {
      const uint8_t* packet = packets.At(place);
      if (!packet) return Read::kBadData;
      const uint32_t first = FirstFrameBit(packet);
      if (first < kPacketBits) {
        place.bit = std::max(place.bit, first);
        return Read::kFrame;
      }
    }
    if (const Read step = packets.Step(place, skipped); step != Read::kFrame) return step;
    same = true;
  }
  return Read::kBadData;
}

uint32_t Offset(const Place& place) { return place.packet * kPacketBits + place.bit; }
}  // namespace

Read ReadFrame(const InputBuffer (&buffers)[2], Cursor at, uint8_t* out, Frame& frame) {
  frame = {};
  const Packets packets(buffers, at.buffer);
  const InputBuffer& current = buffers[at.buffer & 1];
  if (!current.valid || !current.data || current.packets == 0) return Read::kNeedInput;
  Place place{at.buffer & 1, at.offset / kPacketBits, at.offset % kPacketBits};
  if (place.packet >= current.packets) return Read::kEnd;
  uint32_t skipped = 0;
  // Bits before the packet's first frame are an earlier frame's tail.
  if (const Read found = NextFrameStart(packets, place, true, skipped); found != Read::kFrame)
    return found == Read::kNeedInput ? Read::kEnd : found;
  for (int guard = 0; guard < 4096; ++guard) {
    uint8_t length_bytes[4] = {};
    Place probe = place;
    if (const Read read = Copy(packets, probe, kFrameLengthBits, length_bytes, 0, skipped); read != Read::kFrame)
      return read;
    const uint32_t length = (uint32_t(length_bytes[0]) << 7) | (length_bytes[1] >> 1);
    if (length == 0 || length == kMaximumFrameBits) {
      // No more frames in this packet.
      if (const Read found = NextFrameStart(packets, place, false, skipped); found != Read::kFrame)
        return found == Read::kNeedInput ? Read::kEnd : found;
      continue;
    }
    if (length <= kFrameLengthBits) return Read::kBadData;
    std::memset(out, 0, kFrameBufferBytes);
    Place end = place;
    if (const Read read = Copy(packets, end, length, out, 8, skipped); read != Read::kFrame) return read;
    frame.bits = length;
    const bool more = (out[(8 + length - 1) >> 3] >> (7 - ((8 + length - 1) & 7))) & 1;
    // The trailing bit is 1 when the next frame follows in the same packet. A
    // frame that runs into a later packet ends with 0, and the next one begins
    // right after it, at that packet's first frame (measured on the title's
    // streams: 179 of 179 such frames). A 0 in the packet where the frame
    // began is the last frame there: padding follows.
    const bool crossed = end.buffer != place.buffer || end.packet != place.packet;
    Place next = end;
    Read found = Read::kFrame;
    if (!more || next.bit >= kPacketBits) found = NextFrameStart(packets, next, crossed && next.bit < kPacketBits, skipped);
    if (found == Read::kNeedInput || found == Read::kEnd) {
      // The frame is whole; the next one is in a buffer to come.
      frame.next = {at.buffer ^ 1, kPacketHeaderBits};
      frame.left_buffer = true;
    } else if (found != Read::kFrame) {
      return found;
    } else {
      frame.next = {next.buffer, Offset(next)};
      frame.left_buffer = next.buffer != at.buffer;
    }
    frame.skipped_packets = skipped;
    return Read::kFrame;
  }
  return Read::kBadData;
}

uint32_t SealFrame(uint8_t* out, uint32_t bits) {
  const uint32_t size = 1 + (bits + 7) / 8;
  const uint32_t padding_end = size * 8 - 8 - bits;
  out[0] = uint8_t(padding_end << 2);  // No padding before the frame: it starts at byte 1.
  return size;
}
}  // namespace narutorise::audio::xma
