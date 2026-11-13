// Verification on the title's own sounds (stage 2e of the native audio): the
// streams NARUTORISE_AUDIO_XMA_DUMP saved (one .in per stream, its input
// buffer; the name gives rate, channels, frames and the loop), played again
// through a context (audio/xma/context.h) as the title's XAudio drives one: the
// whole sound in buffer 0, 31 output blocks (30 in stereo), the ring emptied
// after every Kick.
//
// Each stream's output is checked against the same frames decoded one after
// the other (the decoder's own time line):
// - a stream of N frames gives decoder samples [192, 192 + 512 x (N - 1)):
//   the start's 192 are the decoder's lead over the hardware, the last frame's
//   tail is never final;
// - a loop gives the stream up to its end (subframe loop_subframe_end of the
//   frame before loop_end), then the loop's body (from subframe
//   loop_subframe_skip of the frame at loop_start) again and again, sample
//   for sample;
// - no frame fails to decode and none is unreadable.
// Game data: the folder stays out of the repository. With no folder the test
// has nothing to check and says so (exit 0 only with --allow-empty).
//
//   narutorise_audio_xma_trace_test <folder> [--out <folder>] [--allow-empty]
// --out writes each stream's output (16-bit big-endian) there, for listening.
#include "audio/xma/context.h"
#include "audio/xma/decoder.h"
#include "audio/xma/stream.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <regex>
#include <string>
#include <vector>

using namespace narutorise::audio::xma;
namespace fs = std::filesystem;

namespace {
constexpr uint32_t kContextAt = 0x1000;
constexpr uint32_t kRingAt = 0x2000;
constexpr uint32_t kInputAt = 0x10000;

class FakeMemory final : public GuestMemory {
 public:
  explicit FakeMemory(size_t size) : bytes(size) {}
  uint8_t* Physical(uint32_t address, uint32_t size) override {
    if (address > bytes.size() || size > bytes.size() - address) return nullptr;
    return bytes.data() + address;
  }
  std::vector<uint8_t> bytes;
};

struct Stream {
  fs::path path;
  int rate = 0, channels = 0, frames = 0;
  bool loop = false;
  uint32_t loop_count = 0, loop_start = 0, loop_end = 0, subframe_end = 0, subframe_skip = 0, at = kPacketHeaderBits;
};

bool Parse(const fs::path& path, Stream& s) {
  static const std::regex name(
      R"(stream-\d+-(\d+)-(\d)ch-(\d+)f(?:-loop(\d+)-(\d+)-(\d+)-e(\d)s(\d)-at(\d+))?\.in)");
  std::smatch m;
  const std::string file = path.filename().string();
  if (!std::regex_match(file, m, name)) return false;
  s.path = path;
  s.rate = std::stoi(m[1]);
  s.channels = std::stoi(m[2]);
  s.frames = std::stoi(m[3]);
  if (m[4].matched) {
    s.loop = true;
    s.loop_count = std::stoul(m[4]);
    s.loop_start = std::stoul(m[5]);
    s.loop_end = std::stoul(m[6]);
    s.subframe_end = std::stoul(m[7]);
    s.subframe_skip = std::stoul(m[8]);
    s.at = std::stoul(m[9]);
  }
  return true;
}

int RateIndex(int rate) {
  for (int i = 0; i < 4; ++i)
    if (kSampleRates[i] == rate) return i;
  return -1;
}

// The frames one after the other, from the start: their places and the decoder's samples.
struct Linear {
  std::vector<Cursor> starts;
  std::vector<int16_t> samples;  // Interleaved, host order.
  int failures = 0;
};
Linear DecodeLinear(const std::vector<uint8_t>& input, const Stream& s) {
  Linear linear;
  InputBuffer buffers[2] = {{input.data(), uint32_t(input.size() / kPacketBytes), true}, {}};
  std::vector<uint8_t> frame(kFrameBufferBytes), pcm(kSamplesPerFrame * 2 * 2);
  Decoder decoder;
  Cursor at{0, kPacketHeaderBits};
  for (;;) {
    Frame f;
    if (ReadFrame(buffers, at, frame.data(), f) != Read::kFrame) break;
    linear.starts.push_back(at);
    if (!decoder.Decode(frame.data(), SealFrame(frame.data(), f.bits), s.rate, s.channels, pcm.data())) {
      ++linear.failures;
      std::fill(pcm.begin(), pcm.end(), uint8_t(0));
    }
    for (uint32_t i = 0; i < kSamplesPerFrame * uint32_t(s.channels); ++i)
      linear.samples.push_back(int16_t((pcm[i * 2] << 8) | pcm[i * 2 + 1]));
    if (f.left_buffer) break;
    at = f.next;
  }
  return linear;
}

struct Played {
  std::vector<int16_t> samples;
  Counters counters;
  uint32_t kicks = 0;
};
// Kicks until `limit` samples per channel are out, or nothing more comes.
void Play(const std::vector<uint8_t>& input, const Stream& s, uint64_t limit, Played& played) {
  FakeMemory memory(kInputAt + input.size());
  std::copy(input.begin(), input.end(), memory.bytes.begin() + kInputAt);
  const uint32_t ring = s.channels == 2 ? 30 : 31;
  ContextData d{};
  d.input_buffer_0_ptr = kInputAt;
  d.input_buffer_0_packet_count = uint32_t(input.size() / kPacketBytes);
  d.input_buffer_0_valid = 1;
  d.input_buffer_read_offset = s.at;
  d.output_buffer_ptr = kRingAt;
  d.output_buffer_block_count = ring;
  d.output_buffer_valid = 1;
  d.subframe_decode_count = 8;
  d.sample_rate = uint32_t(RateIndex(s.rate));
  d.is_stereo = s.channels == 2;
  if (s.loop) {
    d.loop_count = s.loop_count;
    d.loop_start = s.loop_start;
    d.loop_end = s.loop_end;
    d.loop_subframe_end = s.subframe_end;
    d.loop_subframe_skip = s.subframe_skip;
  }
  uint8_t* data = memory.bytes.data() + kContextAt;
  d.Store(data);
  auto context = std::make_unique<Context>();
  context->Reset();
  for (int idle = 0; idle < 3 && played.samples.size() < limit * s.channels;) {
    context->Kick(data, memory, played.counters);
    ++played.kicks;
    ContextData now = ContextData::Load(data);
    uint32_t read = now.output_buffer_read_offset;
    if (read == now.output_buffer_write_offset) {
      ++idle;
      continue;
    }
    idle = 0;
    for (; read != now.output_buffer_write_offset; read = (read + 1) % ring) {
      const uint8_t* block = memory.bytes.data() + kRingAt + read * kOutputBlockBytes;
      for (uint32_t i = 0; i < kOutputBlockBytes; i += 2) played.samples.push_back(int16_t((block[i] << 8) | block[i + 1]));
    }
    now.output_buffer_read_offset = read;
    now.Store(data);
  }
}

// Samples [from, to) per channel of the decoder's time line.
std::vector<int16_t> Span(const Linear& linear, int channels, int64_t from, int64_t to) {
  if (from < 0 || to * channels > int64_t(linear.samples.size()) || to < from) return {};
  return {linear.samples.begin() + from * channels, linear.samples.begin() + to * channels};
}

int failures = 0;
void Check(bool value, const std::string& what) {
  if (!value) {
    ++failures;
    std::cout << "  FAIL " << what << "\n";
  }
}
// Where `got` first differs from `want` over `want`'s length, or -1.
int64_t FirstDifference(const std::vector<int16_t>& got, size_t at, const std::vector<int16_t>& want) {
  if (at + want.size() > got.size()) return int64_t(got.size());
  for (size_t i = 0; i < want.size(); ++i)
    if (got[at + i] != want[i]) return int64_t(i);
  return -1;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: narutorise_audio_xma_trace_test <folder> [--out <folder>] [--allow-empty]\n";
    return 2;
  }
  const fs::path folder = argv[1];
  fs::path out;
  bool allow_empty = false;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--out" && i + 1 < argc) out = argv[++i];
    if (arg == "--allow-empty") allow_empty = true;
  }
  std::vector<Stream> streams;
  if (fs::is_directory(folder))
    for (const auto& entry : fs::directory_iterator(folder)) {
      Stream s;
      if (entry.path().extension() == ".in" && Parse(entry.path(), s)) streams.push_back(s);
    }
  if (streams.empty()) {
    std::cout << "no streams in " << folder.string() << ": nothing checked\n";
    return allow_empty ? 0 : 1;
  }
  if (!out.empty()) fs::create_directories(out);
  int looped = 0;
  for (const Stream& s : streams) {
    std::ifstream file(s.path, std::ios::binary);
    std::vector<uint8_t> input((std::istreambuf_iterator<char>(file)), {});
    const Linear linear = DecodeLinear(input, s);
    const int64_t n = int64_t(linear.starts.size());
    const int failed_before = failures;
    std::cout << s.path.filename().string() << ": " << n << " frames\n";
    Check(linear.failures == 0, "the frames decode one after the other");
    Check(RateIndex(s.rate) >= 0 && (s.channels == 1 || s.channels == 2), "a rate and channels of the title's");
    Check(s.at == kPacketHeaderBits && linear.starts.size() > 2, "a stream read from its start, of 3 frames or more");
    if (failures != failed_before) continue;

    Played played;
    if (!s.loop) {
      Play(input, s, UINT64_MAX, played);
      const auto want = Span(linear, s.channels, Context::kDecoderLead, Context::kDecoderLead + 512 * (n - 1));
      Check(played.samples.size() == want.size(),
            "512 x (N - 1) samples: got " + std::to_string(played.samples.size() / s.channels) + ", want " +
                std::to_string(want.size() / s.channels));
      const int64_t at = FirstDifference(played.samples, 0, want);
      Check(at < 0, "the decoder's samples from 192 (first difference at " + std::to_string(at) + ")");
    } else {
      ++looped;
      // The frames at loop_start and loop_end.
      int64_t first = -1, end_frame = -1;
      for (int64_t i = 0; i < n; ++i) {
        if (linear.starts[i].offset == s.loop_start) first = i;
        if (linear.starts[i].offset == s.loop_end) end_frame = i;
      }
      Check(first >= 0 && end_frame > first, "loop_start and loop_end are frame starts, in order");
      if (failures != failed_before) continue;
      const int64_t stop = 512 * end_frame + 128 * int64_t(s.subframe_end) - int64_t(Context::kDecoderLead);
      const int64_t restart = 512 * first + 128 * int64_t(s.subframe_skip) + int64_t(Context::kDecoderLead);
      const auto opening = Span(linear, s.channels, Context::kDecoderLead, stop);
      const auto body = Span(linear, s.channels, restart, stop);
      Check(!opening.empty() && !body.empty(), "the loop's samples are in the stream");
      if (failures != failed_before) continue;
      const int passes = 3;
      Play(input, s, (opening.size() + passes * body.size()) / s.channels, played);
      int64_t at = FirstDifference(played.samples, 0, opening);
      Check(at < 0, "the stream up to the loop's end (first difference at " + std::to_string(at) + ")");
      for (int pass = 0; pass < passes && failures == failed_before; ++pass) {
        at = FirstDifference(played.samples, opening.size() + pass * body.size(), body);
        Check(at < 0, "loop pass " + std::to_string(pass + 1) + " is the loop's body (first difference at " +
                          std::to_string(at) + ")");
      }
      Check(played.counters.loops.load() >= uint64_t(passes), "the context counted the loops");
      std::cout << "  loop: frames " << first << ".." << end_frame << ", opening " << opening.size() / s.channels
                << " samples, body " << body.size() / s.channels << " (512 x " << (body.size() / s.channels) / 512.0
                << ")\n";
    }
    Check(played.counters.decode_failures.load() == 0 && played.counters.bad_data.load() == 0 &&
              played.counters.bad_addresses.load() == 0,
          "no decode failure, unreadable data or bad address");
    if (!out.empty()) {
      std::ofstream pcm(out / (s.path.stem().string() + ".pcm"), std::ios::binary);
      for (int16_t v : played.samples) {
        const char be[2] = {char(uint16_t(v) >> 8), char(v & 0xFF)};
        pcm.write(be, 2);
      }
    }
    std::cout << "  " << (failures == failed_before ? "ok" : "FAILED") << ": " << played.samples.size() / s.channels
              << " samples in " << played.kicks << " Kicks\n";
  }
  std::cout << streams.size() << " streams (" << looped << " looping), " << failures << " failures\n";
  return failures == 0 ? 0 : 1;
}
