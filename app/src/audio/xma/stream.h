#pragma once

// Reading XMA frames out of a context's input packets (stage 2c of the native
// audio), from the XMA2 packet format as FFmpeg's own XMA2 parser reads it
// (wmaprodec.c decode_packet), not from the SDK.
//
// A context has two input buffers of 2048-byte packets; this title uses them
// in two ways (nota-de-desenho-2a.md): DARE streams put one packet in each and
// refill whichever is consumed; the XAudio voices put the whole sound in buffer
// 0 and never use buffer 1. The packets of a context form one sequence: the
// current buffer's packets, then the other buffer's if it is valid.
//
// A packet: a 32-bit header (6 bits not used here: in the title's streams they
// do not count the frames beginning in the packet, 19 of 1040 packets
// measured; 15 bits of the previous frame's tail at the start of the payload,
// 3 bits metadata, 8 bits packets to skip to this stream's next packet), then
// 16352 payload bits.
// A frame: a 15-bit length (counting itself) and its bits, possibly across
// several packets' payloads, its length field included (the title's streams
// have frames that begin fewer than 15 bits before a packet's end: the frame
// before says another follows, and the next packet's tail field ends where
// that frame does). Its last bit says whether another frame follows at once
// in the same packet. A length of 0 or 0x7FFF ends the packet's frames.
#include <cstdint>

namespace narutorise::audio::xma {
constexpr uint32_t kPacketBytes = 2048;
constexpr uint32_t kPacketBits = kPacketBytes * 8;
constexpr uint32_t kPacketHeaderBits = 32;
constexpr uint32_t kFrameLengthBits = 15;
constexpr uint32_t kMaximumFrameBits = 0x7FFF;
// A frame for the decoder: one byte of padding lengths, the frame, and the 64
// zero bytes FFmpeg's bit reader may look at past the end.
constexpr uint32_t kFrameBufferBytes = 1 + kMaximumFrameBits / 8 + 1 + 64;

struct InputBuffer {
  const uint8_t* data = nullptr;  // Host address of the first packet, or null.
  uint32_t packets = 0;
  bool valid = false;
};

// A place in the input: the buffer (0 or 1) and the bit offset in it, headers
// included (the context's input_buffer_read_offset).
struct Cursor {
  uint32_t buffer = 0;
  uint32_t offset = kPacketHeaderBits;
};

enum class Read {
  kFrame,      // A frame was read; `next` is where the following one starts.
  kNeedInput,  // The frame (or the way to the next one) runs into a buffer not yet valid.
  kEnd,        // No more frames in the valid buffers.
  kBadData,    // A packet that cannot be read (a skip past the buffer, a frame too long).
};

struct Frame {
  uint32_t bits = 0;  // Frame length, including its length field.
  Cursor next;
  // Buffers the frame and the way to the next one went through to the end:
  // the reader passed into the other buffer (the current one is consumed).
  bool left_buffer = false;
  uint32_t skipped_packets = 0;  // Packets jumped by a nonzero skip count.
};

// Reads the frame at `at` into `out` (kFrameBufferBytes, the decoder's packet:
// byte 0 for the padding lengths, the frame from byte 1, bit 0) and finds the
// next frame's place. Does not change the buffers.
Read ReadFrame(const InputBuffer (&buffers)[2], Cursor at, uint8_t* out, Frame& frame);

// The decoder packet's size in bytes for a frame of `bits`, with byte 0 set.
uint32_t SealFrame(uint8_t* out, uint32_t bits);
}  // namespace narutorise::audio::xma
