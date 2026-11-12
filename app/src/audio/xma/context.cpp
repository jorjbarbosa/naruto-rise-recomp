// One XMA context's work (context.h).
#include "audio/xma/context.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace narutorise::audio::xma {
namespace {
// A subframe: the output's unit of time, and of the loop's end and restart.
constexpr uint32_t kSubframeSamples = 128;

void Max(std::atomic<uint64_t>& target, uint64_t value) {
  uint64_t seen = target.load(std::memory_order_relaxed);
  while (value > seen && !target.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
  }
}
}  // namespace

namespace {
std::string dump_folder;
bool dump_loops_only = false;
std::atomic<int> dump_left{0};
std::atomic<int> dump_index{0};
std::atomic<bool> stream_trace{false};
}  // namespace

void SetStreamTrace(bool on) { stream_trace.store(on); }

void SetDump(const std::string& folder, int streams, bool loops_only) {
  dump_folder = folder;
  dump_loops_only = loops_only;
  dump_left.store(streams);
  stream_trace.store(true);
}

void Context::Reset() {
  if (dump_) {
    std::fclose(dump_);
    dump_ = nullptr;
  }
  stream_ = {};
  Active().Reset();
  waiting_ = provisional_ = 0;
  drop_ = kDecoderLead;
}

void Context::Queue(uint32_t from, uint32_t to, int channels) {
  const uint32_t frame_bytes = uint32_t(channels) * 2;
  const uint32_t room = uint32_t(waiting_pcm_.size()) / frame_bytes - waiting_;
  to = std::min(to, from + room);  // Never more than a lookahead, a frame and a block are kept.
  if (to <= from) return;
  std::memcpy(waiting_pcm_.data() + size_t(waiting_) * frame_bytes, pcm_.data() + size_t(from) * frame_bytes,
              size_t(to - from) * frame_bytes);
  waiting_ += to - from;
}

uint32_t Context::Drain(uint8_t* ring, uint32_t ring_blocks, uint32_t& write, uint32_t room, int channels) {
  const uint32_t frame_bytes = uint32_t(channels) * 2;
  const uint32_t final_bytes = (waiting_ - provisional_) * frame_bytes;
  const uint32_t count = std::min(room, final_bytes / kOutputBlockBytes);
  for (uint32_t i = 0; i < count; ++i) {
    std::memcpy(ring + size_t(write) * kOutputBlockBytes, waiting_pcm_.data() + size_t(i) * kOutputBlockBytes,
                kOutputBlockBytes);
    write = (write + 1) % ring_blocks;
  }
  if (dump_ && count) std::fwrite(waiting_pcm_.data(), kOutputBlockBytes, count, dump_);
  const uint32_t bytes = count * kOutputBlockBytes;
  std::memmove(waiting_pcm_.data(), waiting_pcm_.data() + bytes, size_t(waiting_) * frame_bytes - bytes);
  waiting_ -= bytes / frame_bytes;
  stream_.blocks += count;
  return count;
}

void Context::Kick(uint8_t* data, GuestMemory& memory, Counters& counters) {
  const auto started = std::chrono::steady_clock::now();
  counters.kicks.fetch_add(1, std::memory_order_relaxed);
  ContextData d = ContextData::Load(data);
  struct Timer {
    std::chrono::steady_clock::time_point started;
    Counters& counters;
    ~Timer() {
      const auto ns = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
      counters.kick_ns_total.fetch_add(ns, std::memory_order_relaxed);
      Max(counters.kick_ns_max, ns);
      if (ns > 1'000'000) counters.kicks_over_1ms.fetch_add(1, std::memory_order_relaxed);
      if (ns > 5'333'333) counters.kicks_over_budget.fetch_add(1, std::memory_order_relaxed);
    }
  } timer{started, counters};

  if (!d.output_buffer_valid) {
    counters.kicks_without_output.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (d.stop_when_done || d.interrupt_when_done || d.output_buffer_padding ||
      (d.loop_count != 0 && d.loop_count != 255))
    counters.outside_shape.fetch_add(1, std::memory_order_relaxed);

  const uint32_t ring_blocks = d.output_buffer_block_count;
  uint8_t* ring = ring_blocks ? memory.Physical(d.output_buffer_ptr, ring_blocks * kOutputBlockBytes) : nullptr;
  if (!ring) {
    counters.bad_addresses.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const int channels = d.is_stereo ? 2 : 1;
  const int rate = kSampleRates[d.sample_rate];
  uint32_t write = d.output_buffer_write_offset % ring_blocks;
  const uint32_t read = d.output_buffer_read_offset % ring_blocks;
  // Read and write meeting is always an empty ring to the consumers: DARE
  // delivers nothing when they meet (sub_822B96E8). So one block always stays
  // free and the write offset never reaches the read offset.
  uint32_t room = (read + ring_blocks - write - 1) % ring_blocks;

  InputBuffer input[2];
  const uint32_t counts[2] = {d.input_buffer_0_packet_count, d.input_buffer_1_packet_count};
  const uint32_t addresses[2] = {d.input_buffer_0_ptr, d.input_buffer_1_ptr};
  bool valid[2] = {bool(d.input_buffer_0_valid), bool(d.input_buffer_1_valid)};
  for (int b = 0; b < 2; ++b) {
    if (!valid[b]) continue;
    const uint8_t* bytes = counts[b] ? memory.Physical(addresses[b], counts[b] * kPacketBytes) : nullptr;
    if (!bytes) {
      // Nothing to read there: the buffer is given back as consumed.
      counters.bad_addresses.fetch_add(1, std::memory_order_relaxed);
      valid[b] = false;
      continue;
    }
    input[b] = {bytes, counts[b], true};
  }
  Cursor cursor{d.current_buffer, d.input_buffer_read_offset};
  ++stream_.kicks;
  stream_.rate = rate;
  stream_.channels = channels;
  stream_.ring = ring_blocks;
  stream_.loop = d.loop_count != 0;
  if (stream_.frames_in_input < 0 && valid[cursor.buffer] && stream_trace.load(std::memory_order_relaxed)) {
    // The frames this stream's input holds from here, for the trace.
    stream_.packets = input[cursor.buffer].packets;
    int32_t count = 0;
    Cursor probe = cursor;
    Frame frame;
    for (; count < 100000; ++count) {
      if (ReadFrame(input, probe, frame_.data(), frame) != Read::kFrame) break;
      if (frame.left_buffer) {
        ++count;
        break;
      }
      probe = frame.next;
    }
    stream_.frames_in_input = count;
    if (!dump_folder.empty() && (!dump_loops_only || d.loop_count) && dump_left.fetch_sub(1) > 0) {
      const int n = dump_index.fetch_add(1);
      std::string stem = dump_folder + "/stream-" + std::to_string(n) + "-" + std::to_string(rate) + "-" +
                         std::to_string(channels) + "ch-" + std::to_string(stream_.frames_in_input) + "f";
      if (d.loop_count)
        stem += "-loop" + std::to_string(d.loop_count) + "-" + std::to_string(d.loop_start) + "-" +
                std::to_string(d.loop_end) + "-e" + std::to_string(d.loop_subframe_end) + "s" +
                std::to_string(d.loop_subframe_skip) + "-at" + std::to_string(cursor.offset);
      if (std::FILE* in = std::fopen((stem + ".in").c_str(), "wb")) {
        std::fwrite(input[cursor.buffer].data, kPacketBytes, input[cursor.buffer].packets, in);
        std::fclose(in);
      }
      dump_ = std::fopen((stem + ".pcm").c_str(), "wb");
    }
  }
  const auto consume = [&](uint32_t buffer) {
    valid[buffer] = false;
    input[buffer].valid = false;
    if (!valid[buffer ^ 1]) stream_.consumed = true;
    counters.buffers_consumed.fetch_add(1, std::memory_order_relaxed);
  };

  const uint32_t frame_bytes = uint32_t(channels) * 2;
  for (int guard = 0; guard < 64 && room > 0; ++guard) {
    if ((waiting_ - provisional_) * frame_bytes >= kOutputBlockBytes) {
      const uint32_t written = Drain(ring, ring_blocks, write, room, channels);
      room -= written;
      counters.blocks.fetch_add(written, std::memory_order_relaxed);
      continue;
    }
    if (!valid[cursor.buffer]) {
      if (!valid[cursor.buffer ^ 1]) break;  // Nothing to decode until the title gives input.
      cursor = {cursor.buffer ^ 1, kPacketHeaderBits};
    }
    // The frame at loop_end closes the loop: the loop's last frame ended there.
    const bool loop_end_frame = d.loop_count != 0 && cursor.buffer == 0 && cursor.offset == d.loop_end;
    Frame frame;
    const Read read_result = ReadFrame(input, cursor, frame_.data(), frame);
    if (read_result == Read::kNeedInput) {
      counters.stalls.fetch_add(1, std::memory_order_relaxed);
      ++stream_.stalls;
      break;
    }
    if (read_result == Read::kEnd || read_result == Read::kBadData) {
      if (read_result == Read::kBadData) counters.bad_data.fetch_add(1, std::memory_order_relaxed);
      consume(cursor.buffer);
      if (!valid[cursor.buffer ^ 1]) {
        counters.streams_ended.fetch_add(1, std::memory_order_relaxed);
        cursor = {cursor.buffer ^ 1, kPacketHeaderBits};
        break;
      }
      cursor = {cursor.buffer ^ 1, kPacketHeaderBits};
      continue;
    }
    counters.frames.fetch_add(1, std::memory_order_relaxed);
    ++stream_.frames;
    counters.skipped_packets.fetch_add(frame.skipped_packets, std::memory_order_relaxed);
    const uint32_t size = SealFrame(frame_.data(), frame.bits);
    if (!Active().Decode(frame_.data(), size, rate, channels, pcm_.data())) {
      // The frame's time is kept, as silence.
      counters.decode_failures.fetch_add(1, std::memory_order_relaxed);
      std::fill(pcm_.begin(), pcm_.end(), uint8_t(0));
    }
    const uint32_t from = std::min(drop_, kSamplesPerFrame);
    drop_ -= from;
    if (loop_end_frame) {
      // The loop's last frame ends at its subframe loop_subframe_end, in
      // decoder time this frame's sample 128 x loop_subframe_end - 192 (64 for
      // the title's 2): a negative one is in the previous frame, still waiting.
      const int32_t end = int32_t(kSubframeSamples * d.loop_subframe_end) - int32_t(kDecoderLead);
      if (end >= 0) {
        Queue(from, std::max<uint32_t>(from, uint32_t(end)), channels);
      } else {
        const uint32_t cut = std::min<uint32_t>(uint32_t(-end), provisional_);
        waiting_ -= cut;
      }
      provisional_ = 0;
      // Then the loop's first frame, from its subframe loop_subframe_skip.
      cursor = {0, d.loop_start};
      drop_ = kSubframeSamples * std::min<uint32_t>(d.loop_subframe_skip, 4) + kDecoderLead;
      if (d.loop_count != 255) --d.loop_count;
      counters.loops.fetch_add(1, std::memory_order_relaxed);
      ++stream_.loops;
      continue;  // Buffer 0 holds the loop: it stays valid.
    }
    Queue(from, kSamplesPerFrame, channels);
    // What the previous frame left waiting is final now; this frame's samples
    // from 192 on wait for the next frame.
    provisional_ = std::min(waiting_, kSamplesPerFrame - std::max(from, kDecoderLead));
    if (frame.left_buffer) consume(cursor.buffer);
    cursor = frame.next;
  }

  d.output_buffer_write_offset = write;
  d.input_buffer_0_valid = valid[0];
  d.input_buffer_1_valid = valid[1];
  d.current_buffer = cursor.buffer;
  d.input_buffer_read_offset = cursor.offset;
  d.Store(data);
}
}  // namespace narutorise::audio::xma
