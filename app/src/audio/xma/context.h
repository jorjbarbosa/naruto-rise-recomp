#pragma once

// One XMA context: the hardware's work on the 64 bytes the title shares with it
// (stage 2c of the native audio), for the two shapes this title uses
// (nota-de-desenho-2a.md):
// - DARE streams: buffers 0 and 1 of one packet each, refilled as they are
//   consumed; 15 output blocks; mono, 48 kHz; no loop.
// - XAudio voices: the whole sound in buffer 0; 30 (stereo) or 31 (mono)
//   output blocks; any of the four rates; no loop, or an endless loop that
//   plays subframes 0-2 of the loop's last frame and skips subframes 0-2 of
//   its first.
//
// A Kick decodes while the output ring has room: frames are read from the
// input (stream.h), decoded (decoder.h) and written in 256-byte blocks (one
// subframe of 128 samples per channel). Read and write offsets meeting is an
// empty ring to the consumers (DARE's sub_822B96E8 delivers nothing then), so
// the ring is never filled to the last block: one stays free. The output's
// valid flag is the consumer's; the context only reads it. Samples that did
// not fit wait for the next Kick. An input buffer is invalidated once its
// frames are all read.
//
// Where a stream's output begins and ends, and where a loop closes, measured
// on the title's sounds (out/native-audio-20261111/etapa2/loop-native-1):
// the hardware's output is the decoder's 192 samples earlier. Every sound
// begins at decoder sample 576 (silence before it), which is subframe 3 of
// frame 0, the restart point of every loop the title has (skip 3). The loop's
// last frame is the one that ends at loop_end; its subframe 2 ends 64 samples
// into the frame at loop_end, the stream's last, which is there only to
// finish it: the last 320 samples of each decoded frame wait for the next one
// and, at a stream's end, are never written. So a stream of N frames gives
// 512 x (N - 1) samples, and a loop from frame 0 to loop_end 512 x (N - 2).
//
// Anything outside those shapes is counted. A bad address (input or output
// outside guest physical memory) stops the context's work and is counted;
// nothing reaches the host as an access fault.
#include "audio/xma/decoder.h"
#include "audio/xma/format.h"
#include "audio/xma/stream.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace narutorise::audio::xma {
// The physical address stored for a buffer whose virtual address did not
// translate (xma_runtime.cpp): outside guest physical memory, so a Kick finds
// no memory there and counts a bad address. Not 0: physical 0 is real memory,
// and the ring would be written over whatever the title keeps there.
constexpr uint32_t kBadAddress = 0xFFFFFFFF;

class GuestMemory {
 public:
  virtual ~GuestMemory() = default;
  // The host address of guest physical [address, address + size), or null
  // when that range is not guest physical memory (always for kBadAddress).
  virtual uint8_t* Physical(uint32_t address, uint32_t size) = 0;
};

struct Counters {
  std::atomic<uint64_t> kicks{0}, kicks_without_output{0}, frames{0}, blocks{0}, decode_failures{0},
      bad_data{0}, bad_addresses{0}, stalls{0}, buffers_consumed{0}, streams_ended{0}, loops{0},
      skipped_packets{0}, outside_shape{0}, kick_ns_total{0}, kick_ns_max{0},
      // Kicks over 1 ms, and over one audio frame (256 samples at 48 kHz, the budget).
      kicks_over_1ms{0}, kicks_over_budget{0};
};

// One stream's life on a context (from a reset to the next), for the trace.
struct StreamStats {
  uint32_t kicks = 0, frames = 0, blocks = 0, stalls = 0;
  uint32_t packets = 0, rate = 0, channels = 0, ring = 0;
  int32_t frames_in_input = -1;  // Counted at the first Kick with input, with SetStreamTrace.
  bool consumed = false;         // Its input buffers were all read.
  bool loop = false;
  uint32_t loops = 0;            // Times it went back to its loop's start.
};

// NARUTORISE_AUDIO_XMA_DUMP=<folder>: the first `streams` streams' input (the
// current buffer at their first Kick) and the PCM written for them, one pair of
// files each; with `loops_only`, only the streams that loop, the loop's fields
// in the file names. For audio_xma_trace_test. Game data: keep the folder out
// of the repository. Implies SetStreamTrace.
void SetDump(const std::string& folder, int streams, bool loops_only);
// The frames each stream's input holds, counted at its first Kick, for the
// trace and the dump (a walk of the whole input: off unless asked for).
void SetStreamTrace(bool on);

class Context {
 public:
  // The hardware's work on the context at `data` (its 64 guest bytes).
  void Kick(uint8_t* data, GuestMemory& memory, Counters& counters);
  // A new stream: the decoder's history and any waiting samples are dropped.
  void Reset();
  // Tests: decode with `decoder` instead of the app's FFmpeg (null: FFmpeg again).
  void UseDecoder(FrameDecoder* decoder) { decoder_override_ = decoder; }
  std::mutex& Mutex() { return mutex_; }
  const StreamStats& Stream() const { return stream_; }
  // Decoded samples (per channel) not yet written to the ring.
  uint32_t Pending() const { return waiting_; }

  // The decoder's output is the hardware's this many samples later.
  static constexpr uint32_t kDecoderLead = 192;
  // The samples of the newest decoded frame that are final only with the next one.
  static constexpr uint32_t kLookahead = kSamplesPerFrame - kDecoderLead;

 private:
  // Adds decoded samples [from, to) of `pcm_` to the waiting ones.
  void Queue(uint32_t from, uint32_t to, int channels);
  // Writes final waiting samples to the ring, whole blocks; returns the blocks written.
  uint32_t Drain(uint8_t* ring, uint32_t ring_blocks, uint32_t& write, uint32_t room, int channels);

  FrameDecoder& Active() { return decoder_override_ ? *decoder_override_ : decoder_; }

  std::mutex mutex_;
  Decoder decoder_;
  FrameDecoder* decoder_override_ = nullptr;
  std::array<uint8_t, kFrameBufferBytes> frame_{};
  // One decoded frame: 512 samples x up to 2 channels x 2 bytes.
  std::array<uint8_t, kSamplesPerFrame * 2 * 2> pcm_{};
  // Samples waiting for the ring, channels interleaved: at most a frame's
  // lookahead, a frame, and a block not yet whole.
  std::array<uint8_t, kSamplesPerFrame * 3 * 2 * 2> waiting_pcm_{};
  uint32_t waiting_ = 0;      // Samples (per channel) in waiting_pcm_...
  uint32_t provisional_ = 0;  // ...of which the last ones wait for the next frame.
  uint32_t drop_ = kDecoderLead;  // Decoded samples to drop before the next one kept.
  StreamStats stream_;
  std::FILE* dump_ = nullptr;  // The PCM of this stream, when it is dumped.
};
}  // namespace narutorise::audio::xma
