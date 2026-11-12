// What the title asks of the XMA hardware, measured in play (measure.h).
#include "audio/measure.h"
#include "audio/xma/format.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>
#include <fmt/format.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mmio_handler.h>
#include <rex/system/xmemory.h>

namespace narutorise::audio::measure {
namespace {
std::atomic<bool> enabled{false};

// Voice types by the init's first byte (0-7, 8 = other) and caller region.
constexpr const char* kRegions[] = {"dare", "bink", "lib_8250d", "xaudio", "other"};
constexpr size_t kRegionCount = std::size(kRegions);
std::array<std::array<std::atomic<uint64_t>, kRegionCount>, 9> voices{};
// Decoder formats 0-5 (0 = outside 1-5).
std::array<std::atomic<uint64_t>, 6> formats{};

// The XMA registers: 64 KB of 4-byte registers.
constexpr uint32_t kRegisterBase = 0x7FEA0000;
constexpr uint32_t kRegisterCount = 0x10000 / 4;
std::array<std::atomic<uint32_t>, kRegisterCount> reads{}, writes{};
std::mutex value_mutex;
std::map<uint32_t, uint64_t> values_601;
rex::runtime::MMIOReadCallback sdk_read = nullptr;
rex::runtime::MMIOWriteCallback sdk_write = nullptr;
std::atomic<bool> mmio_wrapped{false};
std::string mmio_state = "not yet";

// Context profiles by path ('A' registers, 'B' imports): distinct profiles
// counted, at most kProfiles of them.
constexpr size_t kProfiles = 64;
std::mutex profile_mutex;
std::map<std::string, uint64_t> profiles[2];
uint64_t profiles_dropped = 0;

// The imports of the gate.
std::mutex import_mutex;
std::vector<const char*> import_names;
std::array<std::atomic<uint64_t>, 64> import_calls{};

size_t Region(uint32_t lr) {
  if (lr >= 0x82290000 && lr < 0x822D0000) return 0;
  if (lr >= 0x826C0000 && lr < 0x826D0000) return 1;
  if (lr >= 0x8250D000 && lr < 0x82515000) return 2;
  if (lr >= 0x82148000 && lr < 0x82160000) return 3;
  return 4;
}

// The context array's physical address, as the title read it from 0x600.
std::atomic<uint32_t> array_physical{0};

uint32_t ReadRegister(void* ppc_context, void* callback_context, uint32_t address) {
  const uint32_t index = (address & 0xFFFF) >> 2;
  reads[index].fetch_add(1, std::memory_order_relaxed);
  const uint32_t value = sdk_read(ppc_context, callback_context, address);
  if (index == 0x600) array_physical.store(std::byteswap(value));  // The register's owner returns it swapped.
  return value;
}

// The contexts a Kick names, as the title set them before the hardware runs.
void NoteKick(uint32_t index, uint32_t value) {
  auto* kernel = REX_KERNEL_STATE();
  const uint32_t base = array_physical.load();
  if (!kernel || !base) return;
  const uint32_t bits = std::byteswap(value);  // The stored word arrives swapped.
  const uint32_t first = (index - 0x650) * 32;
  for (uint32_t bit = 0; bit < 32; ++bit) {
    if (!(bits & (1u << bit))) continue;
    const uint32_t physical = base + (first + bit) * xma::kContextBytes;
    NoteContext('A', kernel->memory()->TranslatePhysical(physical));
  }
}

// NARUTORISE_AUDIO_MEASURE_DUMP=<folder>: the PCM the register owner (the
// SDK or the native XMA) writes for the first 40 streams of path A, one file
// per stream (a Clear starts one), read from the output ring after each Kick.
// For comparing the two decoders on the same sounds. Game data: keep the folder
// out of the repository. With NARUTORISE_AUDIO_DUMP_LOOPS=1, only the streams
// that loop are dumped, the loop's fields in the file name.
struct RingDump {
  std::FILE* file = nullptr;
  uint32_t write = 0;
  bool open = false;  // A Clear started a stream; the file opens at its first Kick.
};
std::string dump_folder;
bool dump_loops_only = false;
std::mutex dump_mutex;
std::array<RingDump, 320> dumps{};
int dump_streams = 0;

uint8_t* ContextAt(uint32_t index) {
  auto* kernel = REX_KERNEL_STATE();
  const uint32_t base = array_physical.load();
  return kernel && base ? kernel->memory()->TranslatePhysical(base + index * xma::kContextBytes) : nullptr;
}

void DumpWrites(uint32_t r, uint32_t value, bool before) {
  if (dump_folder.empty()) return;
  const uint32_t bits = std::byteswap(value);
  const bool clear = r >= 0x6A0 && r <= 0x6A9, kick = r >= 0x650 && r <= 0x659;
  if (!clear && !kick) return;
  if (clear != before) return;  // A Clear opens a file before; a Kick copies after.
  std::lock_guard lock(dump_mutex);
  const uint32_t first = (r - (clear ? 0x6A0 : 0x650)) * 32;
  for (uint32_t bit = 0; bit < 32; ++bit) {
    if (!(bits & (1u << bit)) || first + bit >= dumps.size()) continue;
    RingDump& dump = dumps[first + bit];
    if (clear) {
      if (dump.file) std::fclose(dump.file);
      dump.file = nullptr;
      dump.write = 0;
      dump.open = true;
      continue;
    }
    uint8_t* context = dump.file || dump.open ? ContextAt(first + bit) : nullptr;
    if (!context) continue;
    const xma::ContextData d = xma::ContextData::Load(context);
    if (dump.open) {
      dump.open = false;
      if (dump_streams >= 40 || (dump_loops_only && d.loop_count == 0)) continue;
      const std::string loop =
          d.loop_count ? fmt::format("-loop{}-{}-{}-e{}s{}", uint32_t(d.loop_count), uint32_t(d.loop_start),
                                     uint32_t(d.loop_end), uint32_t(d.loop_subframe_end),
                                     uint32_t(d.loop_subframe_skip))
                       : std::string();
      dump.file = std::fopen(
          fmt::format("{}/ring-{}-ctx{}{}.pcm", dump_folder, dump_streams++, first + bit, loop).c_str(), "wb");
      if (!dump.file) continue;
    }
    const uint32_t ring = d.output_buffer_block_count;
    auto* kernel = REX_KERNEL_STATE();
    if (!ring || !kernel) continue;
    const uint8_t* out = kernel->memory()->TranslatePhysical(d.output_buffer_ptr);
    for (uint32_t w = dump.write; w != d.output_buffer_write_offset % ring; w = (w + 1) % ring)
      std::fwrite(out + w * xma::kOutputBlockBytes, xma::kOutputBlockBytes, 1, dump.file);
    dump.write = d.output_buffer_write_offset % ring;
  }
}

void WriteRegister(void* ppc_context, void* callback_context, uint32_t address, uint32_t value) {
  const uint32_t index = (address & 0xFFFF) >> 2;
  writes[index].fetch_add(1, std::memory_order_relaxed);
  if (index >= 0x650 && index <= 0x659) NoteKick(index, value);
  if (index == 0x601) {
    std::lock_guard lock(value_mutex);
    ++values_601[value];
  }
  DumpWrites(index, value, true);
  sdk_write(ppc_context, callback_context, address, value);
  DumpWrites(index, value, false);
}

// Wraps the SDK's callbacks of the XMA range, once, before the title's first
// access (the title's XAudio is starting; DARE's XMA thread does not exist yet).
void WrapRegisters() {
  if (mmio_wrapped.exchange(true)) return;
  auto* kernel = REX_KERNEL_STATE();
  auto* range = kernel ? kernel->memory()->LookupVirtualMappedRange(kRegisterBase) : nullptr;
  if (!range || !range->read || !range->write) {
    mmio_state = "no XMA range registered";
    REXLOG_WARN("[narutorise] Audio measure: {}; register counts unavailable", mmio_state);
    return;
  }
  sdk_read = range->read;
  sdk_write = range->write;
  range->read = ReadRegister;
  range->write = WriteRegister;
  mmio_state = fmt::format("wrapped 0x{:08X}+0x{:X}", range->address, range->size);
}

std::string Counts(const std::array<std::atomic<uint32_t>, kRegisterCount>& counts) {
  std::string out;
  for (uint32_t i = 0; i < kRegisterCount; ++i) {
    const uint32_t n = counts[i].load(std::memory_order_relaxed);
    if (!n) continue;
    out += fmt::format("{}\"0x{:03X}\":{}", out.empty() ? "" : ",", i, n);
  }
  return out;
}
}  // namespace

void Configure() {
  const char* value = std::getenv("NARUTORISE_AUDIO_MEASURE");
  enabled.store(value && *value && std::strcmp(value, "0") != 0);
  if (const char* dump = std::getenv("NARUTORISE_AUDIO_MEASURE_DUMP"); dump && *dump) dump_folder = dump;
  if (const char* loops = std::getenv("NARUTORISE_AUDIO_DUMP_LOOPS"); loops && *loops && *loops != '0')
    dump_loops_only = true;
  if (enabled.load()) REXLOG_INFO("[narutorise] Audio measure: on (NARUTORISE_AUDIO_MEASURE)");
}

bool Enabled() { return enabled.load(std::memory_order_relaxed); }

// Path B output per stream: the blocks DARE found written, seen through the
// write offsets it reads (XMAGetOutputBufferWriteOffset), from one
// XMAInitializeContext (or creation) of a context to the next or its release.
namespace {
struct StreamOutput {
  uint32_t last_write = 0;
  uint64_t blocks = 0, polls = 0;
  uint32_t sequence = 0;
};
std::mutex stream_mutex;
std::map<uint32_t, StreamOutput> stream_output;
uint32_t stream_sequence = 0;
constexpr uint32_t kDareRing = 15;  // DARE's output blocks (sub_822BF278).

void EndStream(uint32_t context, const char* why) {
  auto it = stream_output.find(context);
  if (it == stream_output.end()) return;
  if (it->second.polls)
    REXLOG_INFO("[narutorise] Audio measure stream B: #{} ctx 0x{:08X} {} after {} blocks ({:.2f} s at 48 kHz mono), "
                "{} polls",
                it->second.sequence, context, why, it->second.blocks, it->second.blocks * 128 / 48000.0,
                it->second.polls);
  stream_output.erase(it);
}
}  // namespace

void AfterImport(const char* name, uint32_t context, uint32_t result) {
  if (!Enabled() || !context) return;
  const std::string_view n(name);
  std::lock_guard lock(stream_mutex);
  if (n == "__imp__XMAGetOutputBufferWriteOffset") {
    auto [it, fresh] = stream_output.try_emplace(context);
    StreamOutput& s = it->second;
    if (fresh) s.sequence = ++stream_sequence;
    s.blocks += (result + kDareRing - s.last_write) % kDareRing;
    s.last_write = result;
    ++s.polls;
  } else if (n == "__imp__XMAInitializeContext") {
    EndStream(context, "reinitialized");
  } else if (n == "__imp__XMAReleaseContext") {
    EndStream(context, "released");
  }
}

void FlushStreams() {
  if (!Enabled()) return;
  std::lock_guard lock(stream_mutex);
  while (!stream_output.empty()) EndStream(stream_output.begin()->first, "open at the close");
}

int RegisterImport(const char* name) {
  std::lock_guard lock(import_mutex);
  if (import_names.size() >= import_calls.size()) return -1;
  import_names.push_back(name);
  return static_cast<int>(import_names.size() - 1);
}

void CountProfile(char path, const std::string& key) {
  std::lock_guard lock(profile_mutex);
  auto& map = profiles[path == 'A' ? 0 : 1];
  if (auto it = map.find(key); it != map.end()) {
    ++it->second;
  } else if (map.size() < kProfiles) {
    map.emplace(key, 1);
  } else {
    ++profiles_dropped;
  }
}

void NoteContext(char path, const uint8_t* context) {
  if (!Enabled() || !context) return;
  const xma::ContextData d = xma::ContextData::Load(context);
  const std::string key = fmt::format(
      "in{}/{} out{} sdc{} pad{} rate{} {} loop{} end{} skip{} stop{} irq{} work{} err{}{}", d.input_buffer_0_packet_count,
      d.input_buffer_1_packet_count, d.output_buffer_block_count, d.subframe_decode_count, d.output_buffer_padding,
      xma::kSampleRates[d.sample_rate], d.is_stereo ? "stereo" : "mono",
      d.loop_count == 0 ? "0" : d.loop_count == 255 ? "inf" : "n", d.loop_subframe_end, d.loop_subframe_skip,
      d.stop_when_done, d.interrupt_when_done, d.work_buffer_ptr ? 1 : 0, d.error_set, d.parser_error_set);
  CountProfile(path, key);
}

void NoteInit(uint32_t guest_init) {
  if (!Enabled() || !guest_init) return;
  auto* kernel = REX_KERNEL_STATE();
  if (!kernel) return;
  // XMA_CONTEXT_INIT: 11 big-endian words, then the loop data (start, end,
  // count, subframe end, subframe skip).
  const uint8_t* init = kernel->memory()->TranslateVirtual<const uint8_t*>(guest_init);
  auto word = [&](uint32_t index) {
    uint32_t value;
    std::memcpy(&value, init + index * 4, 4);
    return std::byteswap(value);
  };
  const uint32_t loop_count = init[52];
  const std::string key = fmt::format(
      "in{}/{} read{} out{} sdc{} ch{} rate{} loop{} end{} skip{} loopstart{} loopend{} work{}", word(1), word(3),
      word(4), word(6), word(8), word(9), word(10) < 4 ? xma::kSampleRates[word(10)] : int(word(10)),
      loop_count == 0 ? "0" : loop_count == 255 ? "inf" : std::to_string(loop_count), init[53], init[54],
      word(11) ? "set" : "0", word(12) ? "set" : "0", word(7) ? 1 : 0);
  CountProfile('B', key);
}

void CountImport(int slot) {
  if (slot >= 0 && Enabled()) import_calls[slot].fetch_add(1, std::memory_order_relaxed);
}

std::string Json() {
  std::string voice_text;
  for (size_t type = 0; type < voices.size(); ++type) {
    std::string regions;
    for (size_t r = 0; r < kRegionCount; ++r) {
      const uint64_t n = voices[type][r].load();
      if (n) regions += fmt::format("{}\"{}\":{}", regions.empty() ? "" : ",", kRegions[r], n);
    }
    if (!regions.empty())
      voice_text += fmt::format("{}\"{}\":{{{}}}", voice_text.empty() ? "" : ",",
                                type < 8 ? std::to_string(type) : std::string("other"), regions);
  }
  std::string format_text;
  for (size_t f = 0; f < formats.size(); ++f) {
    const uint64_t n = formats[f].load();
    if (n) format_text += fmt::format("{}\"{}\":{}", format_text.empty() ? "" : ",", f ? std::to_string(f) : "other", n);
  }
  std::string value_text;
  {
    std::lock_guard lock(value_mutex);
    for (const auto& [value, n] : values_601)
      value_text += fmt::format("{}\"0x{:08X}\":{}", value_text.empty() ? "" : ",", value, n);
  }
  std::string import_text;
  {
    std::lock_guard lock(import_mutex);
    for (size_t i = 0; i < import_names.size(); ++i) {
      const uint64_t n = import_calls[i].load();
      if (n) import_text += fmt::format("{}\"{}\":{}", import_text.empty() ? "" : ",", import_names[i] + 7, n);
    }
  }
  std::string profile_text[2];
  uint64_t dropped;
  {
    std::lock_guard lock(profile_mutex);
    for (int p = 0; p < 2; ++p)
      for (const auto& [key, n] : profiles[p])
        profile_text[p] += fmt::format("{}\"{}\":{}", profile_text[p].empty() ? "" : ",", key, n);
    dropped = profiles_dropped;
  }
  return fmt::format("{{\"voices\":{{{}}},\"decoder_formats\":{{{}}},\"mmio\":\"{}\",\"register_reads\":{{{}}},"
                     "\"register_writes\":{{{}}},\"writes_601\":{{{}}},\"xma_imports\":{{{}}},"
                     "\"profiles_a\":{{{}}},\"profiles_b\":{{{}}},\"profiles_dropped\":{}}}",
                     voice_text, format_text, mmio_state, Counts(reads), Counts(writes), value_text, import_text,
                     profile_text[0], profile_text[1], dropped);
}
}  // namespace narutorise::audio::measure

using namespace narutorise::audio::measure;

// sub_82149198: XAudioCreateSourceVoice(r3 = init, r4 = out voice); the init's
// first byte is the sample type (sub_822A83C0 compares it with 1 and 4).
extern "C" void __imp__sub_82149198(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_82149198) {
  if (Enabled() && ctx.r3.u32) {
    const uint8_t type = base[ctx.r3.u32];
    voices[type < 8 ? type : 8][Region(static_cast<uint32_t>(ctx.lr))].fetch_add(1, std::memory_order_relaxed);
  }
  __imp__sub_82149198(ctx, base);
}

// sub_822B84F0: DARE's stream decoder factory, the format in r5 (1-5).
extern "C" void __imp__sub_822B84F0(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_822B84F0) {
  if (Enabled()) {
    const uint32_t format = ctx.r5.u32;
    formats[format >= 1 && format <= 5 ? format : 0].fetch_add(1, std::memory_order_relaxed);
  }
  __imp__sub_822B84F0(ctx, base);
}

// sub_82148EB8: the title's XAudio initializer that reads 0x600 (the context
// array) before creating its engine: the registers are wrapped first.
extern "C" void __imp__sub_82148EB8(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_82148EB8) {
  if (Enabled()) WrapRegisters();
  __imp__sub_82148EB8(ctx, base);
}
