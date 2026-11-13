// One XMA context's Kick (audio/xma/context.h), on packets built from the
// format and a decoder whose samples say which frame and which sample they
// are, so every sample that reaches the output ring can be checked:
// - where a stream's output begins and ends (192 decoder samples dropped at
//   the start, the last frame's tail never written: 512 x (N - 1) samples);
// - the ring's room: one block always free, nothing written past the reader,
//   the rest waiting for the next Kick; the output's valid flag left alone;
// - the input: buffer 0 invalidated once read; DARE's shape, one packet per
//   buffer, waiting for a buffer not yet valid and going on in the other;
// - loops: the end at subframe loop_subframe_end, the restart at subframe
//   loop_subframe_skip, again and again, buffer 0 kept valid;
// - stereo; bad addresses; a new stream after a reset.
#include "audio/xma/context.h"
#include "audio_xma_test_packets.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
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

constexpr uint32_t kContextAt = 0x1000;
constexpr uint32_t kRingAt = 0x2000;
constexpr uint32_t kInputAt = 0x10000;
constexpr int32_t kLead = int32_t(Context::kDecoderLead);

class FakeMemory final : public GuestMemory {
 public:
  explicit FakeMemory(size_t size) : bytes(size) {}
  uint8_t* Physical(uint32_t address, uint32_t size) override {
    if (address > bytes.size() || size > bytes.size() - address) return nullptr;
    return bytes.data() + address;
  }
  std::vector<uint8_t> bytes;
};

// Sample i of frame f is f x 512 + i on channel 0 and its negative minus 1 on
// channel 1. The frame is known by its length, unique in each test.
int Value(int frame, int sample, int channel) {
  const int v = frame * int(kSamplesPerFrame) + sample;
  return channel == 0 ? v : -v - 1;
}
class FakeDecoder final : public FrameDecoder {
 public:
  explicit FakeDecoder(const std::vector<Spec>& specs) {
    for (size_t f = 0; f < specs.size(); ++f) frames_[specs[f].length] = int(f);
  }
  bool Decode(uint8_t* packet, uint32_t size, int, int channels, uint8_t* out) override {
    ++decodes;
    const uint32_t length = (uint32_t(packet[1]) << 7) | (packet[2] >> 1);
    const auto found = frames_.find(length);
    if (found == frames_.end() || size != 1 + (length + 7) / 8) return false;
    for (uint32_t i = 0; i < kSamplesPerFrame; ++i)
      for (int c = 0; c < channels; ++c) {
        const auto v = uint16_t(int16_t(Value(found->second, int(i), c)));
        out[(i * channels + c) * 2] = uint8_t(v >> 8);
        out[(i * channels + c) * 2 + 1] = uint8_t(v);
      }
    return true;
  }
  void Reset() override { ++resets; }
  int decodes = 0, resets = 0;

 private:
  std::map<uint32_t, int> frames_;
};

// Distinct lengths, about 11 frames per packet.
std::vector<Spec> Frames(int count, uint32_t first = 1400) {
  std::vector<Spec> specs;
  for (int f = 0; f < count; ++f) specs.push_back({first + uint32_t(f) * 7});
  return specs;
}

// The decoder's time line of frames played in `order`, samples [from, to) per channel.
std::vector<int> Line(const std::vector<int>& order, int channels, int64_t from, int64_t to) {
  std::vector<int> out;
  for (int64_t t = from; t < to; ++t) {
    const int frame = order[size_t(t / kSamplesPerFrame)];
    for (int c = 0; c < channels; ++c) out.push_back(Value(frame, int(t % kSamplesPerFrame), c));
  }
  return out;
}
std::vector<int> Range(int first, int last) {
  std::vector<int> order;
  for (int f = first; f <= last; ++f) order.push_back(f);
  return order;
}

struct Setup {
  FakeMemory memory{kInputAt + 16 * kPacketBytes};
  std::unique_ptr<Context> context = std::make_unique<Context>();
  Counters counters;
  int channels = 1;
  uint32_t ring = 31;
  std::vector<int> out;  // What the consumer read, interleaved.

  Setup(FakeDecoder& decoder, const Built& built, int channel_count, uint32_t ring_blocks) {
    channels = channel_count;
    ring = ring_blocks;
    std::copy(built.bytes.begin(), built.bytes.end(), memory.bytes.begin() + kInputAt);
    context->UseDecoder(&decoder);
    context->Reset();
    ContextData d{};
    d.input_buffer_0_ptr = kInputAt;
    d.input_buffer_0_packet_count = uint32_t(built.bytes.size() / kPacketBytes);
    d.input_buffer_0_valid = 1;
    d.input_buffer_read_offset = kPacketHeaderBits;
    d.output_buffer_ptr = kRingAt;
    d.output_buffer_block_count = ring;
    d.output_buffer_valid = 1;
    d.subframe_decode_count = 8;
    d.sample_rate = 3;
    d.is_stereo = channels == 2;
    Store(d);
  }
  uint8_t* Data() { return memory.bytes.data() + kContextAt; }
  ContextData Load() { return ContextData::Load(Data()); }
  void Store(const ContextData& d) { d.Store(Data()); }
  void Kick() { context->Kick(Data(), memory, counters); }
  // The consumer: reads every block written, frees them.
  uint32_t Consume() {
    ContextData d = Load();
    uint32_t blocks = 0;
    for (uint32_t r = d.output_buffer_read_offset; r != d.output_buffer_write_offset; r = (r + 1) % ring, ++blocks) {
      const uint8_t* block = memory.bytes.data() + kRingAt + r * kOutputBlockBytes;
      for (uint32_t i = 0; i < kOutputBlockBytes; i += 2) out.push_back(int16_t((block[i] << 8) | block[i + 1]));
    }
    d.output_buffer_read_offset = d.output_buffer_write_offset;
    Store(d);
    return blocks;
  }
  // Kicks and consumes until `samples` per channel are out or nothing more comes.
  void Play(size_t samples = SIZE_MAX) {
    for (int idle = 0; idle < 3 && out.size() < samples * channels;) {
      Kick();
      idle = Consume() ? 0 : idle + 1;
    }
  }
};

bool Starts(const std::vector<int>& got, size_t at, const std::vector<int>& want) {
  if (at + want.size() > got.size()) return false;
  for (size_t i = 0; i < want.size(); ++i)
    if (got[at + i] != want[i]) return false;
  return true;
}

void OneShot() {
  const int n = 24;  // Three packets, frames across their boundaries.
  const auto specs = Frames(n);
  const Built built = Build(3, specs);
  FakeDecoder decoder(specs);
  Setup s(decoder, built, 1, 31);
  s.Play();
  const auto want = Line(Range(0, n - 1), 1, kLead, kLead + 512 * (n - 1));
  Check(s.out.size() == want.size(), "512 x (N - 1) samples: " + std::to_string(s.out.size()));
  Check(Starts(s.out, 0, want), "From decoder sample 192, the frames in order, the last one's tail not written");
  Check(s.counters.frames.load() == uint64_t(n) && decoder.decodes == n, "Each frame decoded once");
  const ContextData d = s.Load();
  Check(!d.input_buffer_0_valid && !d.input_buffer_1_valid, "Buffer 0 is invalidated once read");
  Check(d.output_buffer_valid, "The output's valid flag is left to the consumer");
  Check(s.context->Pending() == Context::kLookahead, "The last frame's tail is still waiting, never final");
}

void RingRoom() {
  const auto specs = Frames(24);
  const Built built = Build(3, specs);
  FakeDecoder decoder(specs);
  Setup s(decoder, built, 1, 31);
  s.Kick();
  ContextData d = s.Load();
  Check(d.output_buffer_write_offset == 30, "An empty ring of 31 takes 30 blocks: one stays free");
  const int decoded = decoder.decodes;
  s.Kick();
  Check(s.Load().output_buffer_write_offset == 30, "A full ring takes nothing more");
  Check(decoder.decodes == decoded, "And nothing more is decoded while it is full");
  d = s.Load();
  d.output_buffer_read_offset = 10;
  s.Store(d);
  s.Kick();
  d = s.Load();
  Check(d.output_buffer_write_offset == 9, "Ten blocks read, ten written: the write offset wraps to 9");
  Check(d.output_buffer_write_offset != d.output_buffer_read_offset, "The write offset never reaches the reader's");
  // The stream's blocks 0..29 went to ring blocks 0..29, then 30..39 to ring
  // blocks 30, 0, 1, ..., 8: ring blocks 9..29 still hold the first Kick's.
  const auto want = Line(Range(0, 23), 1, kLead, kLead + 40 * 128);
  bool placed = true;
  for (uint32_t b = 0; b < 31; ++b) {
    const uint32_t stream_block = b == 30 ? 30 : b <= 8 ? 31 + b : b;
    const uint8_t* block = s.memory.bytes.data() + kRingAt + b * kOutputBlockBytes;
    for (uint32_t i = 0; i < 128; ++i)
      placed &= int16_t((block[i * 2] << 8) | block[i * 2 + 1]) == want[stream_block * 128 + i];
  }
  Check(placed, "Each block in its place in the ring, the second Kick's wrapping from block 30 to 8");
  d.output_buffer_valid = 0;
  s.Store(d);
  const uint64_t without = s.counters.kicks_without_output.load();
  s.Kick();
  Check(s.counters.kicks_without_output.load() == without + 1 && s.Load().output_buffer_write_offset == 9,
        "No output buffer: the Kick does nothing");
}

void TwoBuffers() {
  // DARE's shape: one packet per buffer, refilled with the stream's next packet
  // once consumed; the ring of 15 blocks.
  const int n = 40;
  const auto specs = Frames(n);
  const Built built = Build(4, specs);
  FakeDecoder decoder(specs);
  Setup s(decoder, built, 1, 15);
  ContextData d = s.Load();
  d.input_buffer_0_packet_count = 1;
  d.input_buffer_1_ptr = kInputAt + kPacketBytes;
  d.input_buffer_1_packet_count = 1;
  d.input_buffer_1_valid = 0;  // Not yet given.
  d.subframe_decode_count = 2;
  s.Store(d);
  for (int i = 0; i < 20 && s.counters.stalls.load() == 0; ++i) {
    s.Kick();
    s.Consume();
  }
  Check(s.counters.stalls.load() >= 1, "A frame running into buffer 1, not yet valid, waits for it");
  Check(s.Load().input_buffer_0_valid, "Buffer 0 is kept while its last frame is not whole");
  uint32_t next_packet = 2;
  uint32_t refills = 0;
  for (int idle = 0; idle < 4;) {
    d = s.Load();
    // The title gives the packet it has next to whichever buffer is free.
    for (int b = 0; b < 2; ++b) {
      const bool valid = b == 0 ? d.input_buffer_0_valid : d.input_buffer_1_valid;
      if (valid) continue;
      if (b == 1 && refills == 0) {
        d.input_buffer_1_valid = 1;  // Packet 1, already set.
        ++refills;
        continue;
      }
      if (next_packet >= 4) continue;
      const uint32_t packet = kInputAt + next_packet++ * kPacketBytes;
      if (b == 0) {
        d.input_buffer_0_ptr = packet;
        d.input_buffer_0_valid = 1;
      } else {
        d.input_buffer_1_ptr = packet;
        d.input_buffer_1_valid = 1;
      }
      ++refills;
    }
    s.Store(d);
    s.Kick();
    idle = s.Consume() ? 0 : idle + 1;
  }
  const auto want = Line(Range(0, n - 1), 1, kLead, kLead + 512 * (n - 1));
  Check(s.out.size() == want.size() && Starts(s.out, 0, want),
        "The stream across four packets in the two buffers, in order: " + std::to_string(s.out.size()));
  Check(s.counters.buffers_consumed.load() == 4, "Each buffer invalidated once its packet is read");
}

void Loop(uint32_t subframe_end, uint32_t subframe_skip) {
  const int n = 20;
  const auto specs = Frames(n);
  const Built built = Build(3, specs);
  FakeDecoder decoder(specs);
  Setup s(decoder, built, 1, 31);
  ContextData d = s.Load();
  d.loop_count = 255;
  d.loop_start = built.starts[0];
  d.loop_end = built.starts[n - 1];
  d.loop_subframe_end = subframe_end;
  d.loop_subframe_skip = subframe_skip;
  s.Store(d);
  // In the decoder's time line: the end at the last frame's (n - 1) sample
  // 128 x end - 192, the restart at frame 0's sample 128 x skip + 192.
  const int64_t stop = 512 * (n - 1) + 128 * int64_t(subframe_end) - kLead;
  const int64_t restart = 128 * int64_t(subframe_skip) + kLead;
  const auto opening = Line(Range(0, n - 1), 1, kLead, stop);
  const auto body = Line(Range(0, n - 1), 1, restart, stop);
  const std::string name = " (end " + std::to_string(subframe_end) + ", skip " + std::to_string(subframe_skip) + ")";
  Check(body.size() == size_t(512 * (n - 1) - 128 * (subframe_skip - subframe_end) - 384),
        "The loop's body: 4 subframes per frame, less the skipped and the ones after the end" + name);
  s.Play(opening.size() + 3 * body.size());
  Check(Starts(s.out, 0, opening), "The stream up to the loop's end" + name);
  for (int pass = 0; pass < 3; ++pass)
    Check(Starts(s.out, opening.size() + pass * body.size(), body),
          "Loop pass " + std::to_string(pass + 1) + " is the loop's body" + name);
  Check(s.counters.loops.load() >= 3 && s.Load().loop_count == 255, "Loops counted; an endless loop stays endless");
  Check(s.Load().input_buffer_0_valid, "The loop keeps buffer 0 valid" + name);
  Check(s.counters.outside_shape.load() == 0, "An endless loop is in the title's shape");
}

void Stereo() {
  const int n = 12;
  const auto specs = Frames(n, 2600);  // Longer frames: two channels.
  const Built built = Build(3, specs);
  FakeDecoder decoder(specs);
  Setup s(decoder, built, 2, 30);
  s.Play();
  const auto want = Line(Range(0, n - 1), 2, kLead, kLead + 512 * (n - 1));
  Check(s.out.size() == want.size() && Starts(s.out, 0, want),
        "Stereo: interleaved, from 192, 512 x (N - 1) samples per channel");
}

void BadAddresses() {
  const auto specs = Frames(8);
  const Built built = Build(1, specs);
  FakeDecoder decoder(specs);
  Setup s(decoder, built, 1, 31);
  ContextData d = s.Load();
  d.output_buffer_ptr = 0xFFFFFF00;
  s.Store(d);
  s.Kick();
  Check(s.counters.bad_addresses.load() == 1 && decoder.decodes == 0, "An output outside memory: nothing done, counted");
  d.output_buffer_ptr = kRingAt;
  d.input_buffer_0_ptr = 0xFFFF0000;
  s.Store(d);
  s.Kick();
  Check(s.counters.bad_addresses.load() == 2 && !s.Load().input_buffer_0_valid,
        "An input outside memory: given back as consumed, counted");
  Check(s.Load().output_buffer_write_offset == 0, "And nothing written");
  // What the runtime stores for a buffer whose address did not translate.
  d.input_buffer_0_ptr = kInputAt;
  d.output_buffer_ptr = kBadAddress;
  s.Store(d);
  s.Kick();
  Check(s.counters.bad_addresses.load() == 3 && decoder.decodes == 0, "An output that did not translate is refused");
  d.output_buffer_ptr = kRingAt;
  d.input_buffer_0_ptr = kBadAddress;
  d.input_buffer_0_valid = 1;
  s.Store(d);
  s.Kick();
  Check(s.counters.bad_addresses.load() == 4 && !s.Load().input_buffer_0_valid && decoder.decodes == 0,
        "An input that did not translate is refused");
  Check(s.Load().output_buffer_write_offset == 0, "And nothing written");
}

void ResetStartsOver() {
  const int n = 16;
  const auto specs = Frames(n);
  const Built built = Build(2, specs);
  FakeDecoder decoder(specs);
  Setup s(decoder, built, 1, 31);
  s.Kick();  // Part of the stream decoded, part of it waiting.
  Check(s.context->Pending() > 0, "Samples wait after a Kick into a ring that filled");
  s.context->Reset();
  Check(s.context->Pending() == 0 && decoder.resets == 2, "A reset drops them and the decoder's history");
  ContextData d = s.Load();
  d.input_buffer_0_valid = 1;
  d.current_buffer = 0;
  d.input_buffer_read_offset = kPacketHeaderBits;
  d.output_buffer_read_offset = d.output_buffer_write_offset = 0;
  s.Store(d);
  s.Play();
  const auto want = Line(Range(0, n - 1), 1, kLead, kLead + 512 * (n - 1));
  Check(s.out.size() == want.size() && Starts(s.out, 0, want), "The new stream starts at its own sample 192");
}
}  // namespace

int main() {
  const std::pair<const char*, void (*)()> cases[] = {
      {"a whole sound in buffer 0", OneShot},
      {"the ring's room", RingRoom},
      {"one packet per buffer", TwoBuffers},
      {"the title's loop (end 2, skip 3)", [] { Loop(2, 3); }},
      {"a loop ending in its last frame (end 0, skip 0)", [] { Loop(0, 0); }},
      {"stereo", Stereo},
      {"bad addresses", BadAddresses},
      {"a reset starts over", ResetStartsOver},
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
