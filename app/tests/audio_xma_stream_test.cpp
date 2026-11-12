// Reading XMA frames out of input packets (audio/xma/stream.h), on packets
// built here from the format: frames inside a packet, across one and two
// packet boundaries (ending with a trailing 0, as the title's do), across the
// two input buffers (DARE's one packet each), a length field across a packet
// boundary, padding after a last frame, the zero-length marker, and the decoder packet's
// padding byte.
#include "audio/xma/stream.h"
#include "audio_xma_test_packets.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace narutorise::audio::xma;
using namespace narutorise::audio::xma::test;

namespace {
int checks = 0;
void Check(bool value, const std::string& message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

bool SameBits(const uint8_t* out, const std::vector<uint8_t>& bits) {
  for (size_t i = 0; i < bits.size(); ++i) {
    const uint64_t at = 8 + i;
    if (((out[at >> 3] >> (7 - (at & 7))) & 1) != bits[i]) return false;
  }
  return true;
}

void InsideAndAcrossPackets() {
  // Frames inside packet 0, one across into packet 1, one longer than a whole
  // payload (across two boundaries), then the last.
  const std::vector<Spec> specs = {{3000}, {9000}, {6000}, {20000}, {1200}};
  Built built = Build(4, specs);
  InputBuffer buffers[2] = {{built.bytes.data(), 4, true}, {}};
  std::vector<uint8_t> out(kFrameBufferBytes);
  Cursor cursor{0, kPacketHeaderBits};
  for (size_t f = 0; f < specs.size(); ++f) {
    Frame frame;
    const Read read = ReadFrame(buffers, cursor, out.data(), frame);
    Check(read == Read::kFrame, "Frame " + std::to_string(f) + " is read");
    Check(frame.bits == specs[f].length, "Frame " + std::to_string(f) + " has its length");
    Check(SameBits(out.data(), built.frames[f]), "Frame " + std::to_string(f) + " has its bits");
    if (f + 1 < specs.size()) {
      Check(frame.next.buffer == 0 && frame.next.offset == built.starts[f + 1],
            "Frame " + std::to_string(f) + " points at the next one");
      Check(!frame.left_buffer, "Still in buffer 0");
    } else {
      // No frame after it in buffer 0 (packet 3 has none): buffer 0 is done.
      Check(frame.left_buffer && frame.next.buffer == 1, "After the last frame, buffer 0 is consumed");
    }
    cursor = frame.next;
  }
  Frame frame;
  Check(ReadFrame(buffers, cursor, out.data(), frame) == Read::kNeedInput,
        "Then there is nothing to read until buffer 1 is given");
}

void PaddingAndMarker() {
  // A frame followed by padding: the next begins at the next packet's first frame.
  Built built = Build(2, {{4000, true}, {5000}});
  InputBuffer buffers[2] = {{built.bytes.data(), 2, true}, {}};
  std::vector<uint8_t> out(kFrameBufferBytes);
  Frame frame;
  Check(ReadFrame(buffers, {0, kPacketHeaderBits}, out.data(), frame) == Read::kFrame, "The padded frame is read");
  Check(frame.next.offset == kPacketBits + kPacketHeaderBits, "The next frame is at packet 1's first frame");
  Check(ReadFrame(buffers, frame.next, out.data(), frame) == Read::kFrame && frame.bits == 5000,
        "The frame after the padding is read");
  // A zero length where a frame would begin: the packet has no more frames.
  Built marked = Build(2, {{4000}, {5000}});
  // Overwrite frame 1 (it starts at packet 0, bit 32 + 4000) with a zero length,
  // and move packet 1's first frame offset past it.
  for (uint32_t i = 0; i < kFrameLengthBits; ++i) {
    const uint64_t at = kPacketHeaderBits + 4000 + i;
    marked.bytes[at >> 3] &= uint8_t(~(0x80 >> (at & 7)));
  }
  InputBuffer zero[2] = {{marked.bytes.data(), 2, true}, {}};
  Check(ReadFrame(zero, {0, kPacketHeaderBits}, out.data(), frame) == Read::kFrame, "The frame before the marker");
  const Read after = ReadFrame(zero, frame.next, out.data(), frame);
  Check(after == Read::kEnd || after == Read::kFrame, "A zero length moves on to the next packet, never loops");
}

void AcrossBuffers() {
  // DARE's shape: one packet per buffer; a frame crosses from buffer 0 into 1.
  Built built = Build(2, {{12000}, {7000}, {3000}});
  InputBuffer buffers[2] = {{built.bytes.data(), 1, true}, {built.bytes.data() + kPacketBytes, 1, false}};
  std::vector<uint8_t> out(kFrameBufferBytes);
  Frame frame;
  Check(ReadFrame(buffers, {0, kPacketHeaderBits}, out.data(), frame) == Read::kFrame, "The first frame is read");
  const Cursor second = frame.next;
  Check(ReadFrame(buffers, second, out.data(), frame) == Read::kNeedInput,
        "A frame running into a buffer not yet valid waits for it");
  buffers[1].valid = true;
  Check(ReadFrame(buffers, second, out.data(), frame) == Read::kFrame, "With the buffer, the frame is read");
  Check(SameBits(out.data(), built.frames[1]), "Its bits cross the buffers intact");
  Check(frame.left_buffer && frame.next.buffer == 1, "Buffer 0 is consumed; the next frame is in buffer 1");
  Check(ReadFrame(buffers, frame.next, out.data(), frame) == Read::kFrame && frame.bits == 3000,
        "The next frame is read from buffer 1");
}

void LengthAcrossPackets() {
  // A frame that begins 8 bits before the end of packet 0: its 15-bit length
  // runs into packet 1, as in the title's streams.
  const uint32_t before = kPayloadBits - 8 - 3000;
  const std::vector<Spec> specs = {{3000}, {before}, {1479}, {1540}};
  Built built = Build(2, specs);
  Check(built.starts[2] == kPacketBits - 8, "Frame 2 begins 8 bits before the packet's end");
  InputBuffer buffers[2] = {{built.bytes.data(), 2, true}, {}};
  std::vector<uint8_t> out(kFrameBufferBytes);
  Frame frame;
  Check(ReadFrame(buffers, {0, built.starts[1]}, out.data(), frame) == Read::kFrame, "The frame before it is read");
  Check(frame.next.offset == built.starts[2], "It points at the frame whose length crosses the packet");
  Check(ReadFrame(buffers, frame.next, out.data(), frame) == Read::kFrame && frame.bits == 1479,
        "The frame whose length crosses the packet is read, with its length");
  Check(SameBits(out.data(), built.frames[2]), "Its bits are whole");
  Check(frame.next.offset == built.starts[3] && !frame.left_buffer, "The next one is packet 1's first frame");
  Check(ReadFrame(buffers, frame.next, out.data(), frame) == Read::kFrame && frame.bits == 1540,
        "And it is read");
}

void DecoderPacket() {
  std::vector<uint8_t> out(kFrameBufferBytes, 0);
  Check(SealFrame(out.data(), 16) == 3 && out[0] == 0, "16 bits: 2 bytes after the header, no end padding");
  Check(SealFrame(out.data(), 21) == 4 && out[0] == (3 << 2), "21 bits: 3 bytes, 3 bits of end padding");
}
}  // namespace

int main() {
  const std::pair<const char*, void (*)()> cases[] = {
      {"inside and across packets", InsideAndAcrossPackets},
      {"padding and the zero-length marker", PaddingAndMarker},
      {"across the two buffers", AcrossBuffers},
      {"a length field across packets", LengthAcrossPackets},
      {"the decoder packet", DecoderPacket},
  };
  for (const auto& [name, test] : cases) {
    try {
      test();
      std::cout << "  ok: " << name << "\n";
    } catch (const std::exception& error) {
      std::cerr << "FAIL in '" << name << "' after " << checks << " checks: " << error.what() << "\n";
      return 1;
    }
  }
  std::cout << "PASS: " << checks << " checks\n";
  return 0;
}
