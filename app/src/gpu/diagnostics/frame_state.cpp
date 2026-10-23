// Frame-state instrument, alike in the own mode and in the reference (Xenos)
// mode, off by default.
//
// REX_NATIVE_STATE_CLOCK=vblank or flip: the title clock. 8217DDD0, the XAPI
// QueryPerformanceCounter, is the only reader of the timebase outside the
// graphics library, so every timer of the title and of its middleware goes
// through it. Once the first Swap gave the device, it returns
// q0 + (v - v0) * step, with v0, q0 the count and the original value at the
// switch: the title sees its time advance only when the count does. vblank: v
// is the vblank count +16532 (60 Hz in both modes), step 833333 ticks of the
// 50 MHz guest timebase (1/60 s, 60.00002 Hz). flip: v is the flip count +16552
// (one per presented frame, 30 Hz at the START), step 1666666 (1/30 s). Before
// the first Swap the original runs, except with flip-frozen: the flip clock
// from a constant q0 = 50000000 (1 s), returned from the first call on, so no
// timer of the title starts on host time; with start-frozen, the time stays at
// q0 until the window began and then advances by the flips since that Swap. The
// callers in the window are counted
// by thread and by the two return addresses (LR and the caller's saved LR).
// REX_NATIVE_STATE_DUMP=a-b (1 <= a, at most 8 frames): the raw inputs of every
// draw of the window frames a to b, in frames/state-kNNNN-draws.bin: per draw
// the words kind, r4, r5, r6, r7, LR, stream count, then per stream the fetch,
// size, endian and the first (up to 16 KiB) bytes, then the 4 KiB of VS
// constants; all words little-endian, guest bytes as stored.
// REX_NATIVE_STATE_RASTER=a-b (1 <= a, at most 8 frames): per draw of the
// window frames a to b, in frames/state-kNNNN-raster.json, the raster inputs as
// the device holds them at the hook entry: kind and arguments, bound color and
// depth surfaces (+12432, +12448), the two shader virtual hashes, the register
// blocks 0x2000 (+10368, with PA_SC_WINDOW_OFFSET and the window scissor at
// +10432 to +10440), 0x2100 (+10444), 0x2200 (+10548) and 0x2300 (+10680, with
// PA_SU_VTX_CNTL at +10688), the VS constants c0 to c3, and for each float
// vertex element (32_32, 32_32_32, 32_32_32_32) the minimum and maximum of its
// first two components over the draw's vertices.
//
// REX_NATIVE_STATE_FRAMES=N (1 to 20000): from the first frame with a START
// candidate, N frames are summarized at the Swap entry into
// frames/state-frames.json (Swap count, vblanks, flips, clock, draws, START
// candidates and the frame digest). The reference mode then exits with 86; the
// own mode stops in order (native-d3d-state-complete, 86).
// REX_NATIVE_STATE_COMPONENTS=M: the per-draw component hashes of the first M
// window frames, to find which input differs between two runs.
// REX_NATIVE_STATE_CAPTURE=a-b: the front buffer of the window frames a to b
// (at most 64): own mode, the own texture (RGBA8) read at the Swap entry;
// reference mode, after the original Swap and BlockOnFence on the frame fence
// (+14924), the guest bytes of the front buffer, which readback_resolve=full
// wrote, with its fetch words.
//
// Draw digest, at the hook entry before the original commit, from inputs both
// modes share: kind and arguments; the two shaders (virtual part, and the
// microcode without the VS fetch words, which the commit patches in place or in
// a ring copy depending on fences); the VS and PS float constants (+0x780 and
// +0x1780, 4 KiB each); the 32 fetch constant slots without their address bits;
// the render-state blocks (+10368, +10444, +10548); the bytes of each vertex
// stream the VS fetches (up to 256 KiB each) and the indices of an indexed draw.
// Texture contents and addresses are not in it.

#include "gpu/common/platform.h"
#include "gpu/diagnostics/frame_state.h"
#include "gpu/diagnostics/diagnostics.h"
#include "gpu/d3d/hooks.h"
#include "gpu/d3d/draw.h"
#include "gpu/shaders/shaders.h"
#include "gpu/diagnostics/start_evidence.h"
#include "gpu/d3d/frame.h"
#include "gpu/common/hash.h"
#include "gpu/common/records.h"
#include "gpu/backend/dispatcher.h"
#include "gpu/common/sha256.h"
#include <bit>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>
#include <rex/system/xmemory.h>

using namespace gpu;

namespace gpu {
struct StateConfig {
  int clock = 0;  // 0 off, 1 vblank, 2 flip, 3 flip-frozen, 4 start-frozen, -1 invalid.
  int64_t frames = 0, components = 0, capture_first = -1, capture_last = -1, dump_first = -1, dump_last = -1,
          raster_first = -1, raster_last = -1;
  bool valid = true;
};
bool StateRange(const char* name, int64_t& first, int64_t& last, unsigned span) {
  const char* v = std::getenv(name);
  if (!v || !*v) return true;
  unsigned a = 0, b = 0;
  char tail = 0;
  if (std::sscanf(v, "%u-%u%c", &a, &b, &tail) != 2 || a > b || b - a >= span) return false;
  first = a;
  last = b;
  return true;
}
int64_t StateEnv(const char* name, int64_t low, int64_t high) {
  const char* v = std::getenv(name);
  if (!v || !*v) return 0;
  char* end = nullptr;
  const long long parsed = std::strtoll(v, &end, 10);
  return end && !*end && parsed >= low && parsed <= high ? parsed : -1;
}
const StateConfig& State() {
  static const StateConfig config = [] {
    StateConfig c;
    if (!Diagnostics()) return c;
    if (const char* v = std::getenv("REX_NATIVE_STATE_CLOCK"); v && *v) {
      const std::string_view s(v);
      c.clock = s == "vblank" ? 1 : s == "flip" ? 2 : s == "flip-frozen" ? 3 : s == "start-frozen" ? 4 : -1;
    }
    c.frames = StateEnv("REX_NATIVE_STATE_FRAMES", 1, 20000);
    c.components = StateEnv("REX_NATIVE_STATE_COMPONENTS", 1, 20000);
    c.valid = StateRange("REX_NATIVE_STATE_CAPTURE", c.capture_first, c.capture_last, 64) &&
              StateRange("REX_NATIVE_STATE_DUMP", c.dump_first, c.dump_last, 8) &&
              StateRange("REX_NATIVE_STATE_RASTER", c.raster_first, c.raster_last, 8);
    c.valid = c.valid && c.clock >= 0 && (c.clock != 4 || c.frames > 0) && c.frames >= 0 && c.components >= 0 && c.components <= c.frames &&
              (c.capture_first < 0 || (c.frames > 0 && c.capture_last < c.frames)) &&
              (c.dump_first < 0 || (c.dump_first >= 1 && c.frames > 0 && c.dump_last < c.frames)) &&
              (c.raster_first < 0 || (c.raster_first >= 1 && c.frames > 0 && c.raster_last < c.frames));
    return c;
  }();
  return config;
}

// Title clock (REX_NATIVE_STATE_CLOCK).
constexpr uint64_t kStateTicksPerVblank = 833333;  // 50 MHz / 60, rounded down.
constexpr uint64_t kStateFrozenQ0 = 50000000;  // flip-frozen and start-frozen: 1 s of the guest timebase.
const char* StateClockName(int mode) {
  return mode == 1 ? "vblank" : mode == 2 ? "flip" : mode == 3 ? "flip-frozen" : mode == 4 ? "start-frozen" : "off";
}
uint32_t StateClockCounter() { return State().clock >= 2 ? 16552 : 16532; }
uint64_t StateClockStep() { return State().clock >= 2 ? 2 * kStateTicksPerVblank : kStateTicksPerVblank; }
std::atomic<uint32_t> state_clock_device{0};
std::atomic<bool> state_clock_on{false}, state_census_on{false};
std::mutex state_clock_mutex;
uint64_t state_clock_q0 = 0;
std::atomic<uint32_t> state_clock_v0{0};
std::atomic<uint64_t> state_clock_calls{0}, state_clock_last{0};
// Callers of the clock in the window: (guest thread, LR, caller's saved LR) -> calls.
std::mutex state_census_mutex;
std::map<std::array<uint32_t, 3>, uint64_t> state_census;
void StateCensus(PPCContext& ctx, uint8_t* base) {
  if (!state_census_on.load(std::memory_order_relaxed)) return;
  const uint32_t sp = ctx.r1.u32, back = sp ? Get32(base, sp) : 0;
  const uint32_t lr2 = back >= 8 && back < 0xF0000000u ? Get32(base, back - 8) : 0;
  std::lock_guard lock(state_census_mutex);
  ++state_census[{GuestThread(), uint32_t(ctx.lr), lr2}];
}
}  // namespace gpu

// vblank and flip: the original until the first Swap gave the device, then
// q0 + (count - v0) * step from the original value and the count at the switch.
// flip-frozen: q0 = 1 s from the first call; from the first Swap on, plus the
// flips since the device creation (+16552 starts at 0 in both modes) * step.
// start-frozen: q0 = 1 s until the window began (first frame with a START
// candidate, StateAtSwap), then plus the flips since that Swap * step.
extern "C" void __imp__sub_8217DDD0(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_8217DDD0) {
  const int mode = State().clock;
  if (mode <= 0) { __imp__sub_8217DDD0(ctx, base); return; }
  const uint32_t dev = state_clock_device.load(std::memory_order_acquire);
  const uint32_t out = ctx.r3.u32;
  uint64_t value = kStateFrozenQ0;
  if (mode >= 3) {
    if (dev) StateCensus(ctx, base);
    if (dev && state_clock_on.load(std::memory_order_acquire))
      value += uint64_t(Get32(base, dev + 16552) - state_clock_v0.load()) * StateClockStep();
  } else {
    if (!dev) { __imp__sub_8217DDD0(ctx, base); return; }
    StateCensus(ctx, base);
    if (!state_clock_on.load(std::memory_order_acquire)) {
      std::lock_guard lock(state_clock_mutex);
      if (!state_clock_on.load()) {
        __imp__sub_8217DDD0(ctx, base);  // The original value at the switch.
        state_clock_q0 = (uint64_t(Get32(base, out)) << 32) | Get32(base, out + 4);
        state_clock_v0.store(Get32(base, dev + StateClockCounter()));
        state_clock_last.store(state_clock_q0);
        state_clock_calls.fetch_add(1);
        state_clock_on.store(true, std::memory_order_release);
        return;
      }
    }
    value = state_clock_q0 + uint64_t(Get32(base, dev + StateClockCounter()) - state_clock_v0.load()) * StateClockStep();
  }
  Put32(base, out, uint32_t(value >> 32));
  Put32(base, out + 4, uint32_t(value));
  state_clock_last.store(value, std::memory_order_relaxed);
  state_clock_calls.fetch_add(1, std::memory_order_relaxed);
  ctx.r3.u64 = 1;
}

namespace gpu {
// Frame being drawn (render thread; the mutex covers a draw from another thread).
constexpr size_t kStateComponents = 9;  // args, vs, ps, vsc, psc, fetch, regs, vb, ib
struct StateFrame {
  uint64_t digest = 0xCBF29CE484222325ull;
  uint32_t draws = 0, red = 0, white = 0;
  std::vector<std::array<uint64_t, kStateComponents>> components;
};
std::mutex state_mutex;
StateFrame state_frame;
std::vector<uint8_t> state_dump;  // REX_NATIVE_STATE_DUMP: raw inputs of the frame being drawn.
std::string state_raster;  // REX_NATIVE_STATE_RASTER: raster inputs of the frame being drawn, one record per draw.
std::atomic<int64_t> state_draw_frame{-1};  // Window index of the frame being drawn, once the window began.
void DumpWords(std::vector<uint8_t>& out, std::initializer_list<uint32_t> words) {
  for (uint32_t w : words) out.insert(out.end(), reinterpret_cast<const uint8_t*>(&w), reinterpret_cast<const uint8_t*>(&w) + 4);
}
uint64_t StateWords(const uint32_t* words, size_t n) { return Fnv64(reinterpret_cast<const uint8_t*>(words), 4 * n); }
uint64_t StateShader(const BoundShader& s, const std::vector<uint32_t>& vfetch_words) {
  std::vector<uint32_t> code = s.code;
  for (uint32_t i : vfetch_words)
    if (i < code.size()) code[i] = 0;
  return s.virtual_hash ^ (StateWords(code.data(), code.size()) * 0x100000001B3ull);
}
// Bytes of a vertex fetch format; 0 when not listed.
uint32_t StateVertexBytes(uint32_t format) {
  switch (format) {
    case 6: case 7: case 16: case 17: case 25: case 31: case 33: case 36: return 4;
    case 26: case 32: case 34: case 37: return 8;
    case 57: return 12;
    case 35: case 38: return 16;
    default: return 0;
  }
}
// One vertex element as the VS fetches it: fetch constant, stride and offset in
// words, format and bytes.
struct StateElement {
  uint32_t fetch, stride, offset, format, bytes;
};
// The VS elements from the container list, as the own draw reads them: three
// microcode words per vertex fetch (their indices go to words) and, from the
// given code, the fetch constant, stride, offset and format of each element.
bool StateVertexElements(uint8_t* base, const BoundShader& vs, const std::vector<uint32_t>& code,
                         std::vector<uint32_t>& words, std::vector<StateElement>& elements) {
  const uint32_t shader = vs.container + Get32(base, vs.container + 0x18);
  const uint32_t field18 = Get32(base, shader + 0x18), count = Get32(base, shader + 0x1C);
  if (count > 16) return false;
  uint32_t last = UINT32_MAX, last_stride = 0;
  std::vector<std::pair<uint32_t, uint32_t>> ordered;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t value = Get32(base, shader + 0x24 + 4 * (field18 + i));
    ordered.emplace_back(value & 0xFFF, value);
  }
  std::sort(ordered.begin(), ordered.end());
  for (const auto& [address, value] : ordered) {
    if (3 * address + 2 >= code.size()) return false;
    const uint32_t w0 = code[3 * address], w1 = code[3 * address + 1], w2 = code[3 * address + 2];
    for (uint32_t k = 0; k < 3; ++k) words.push_back(3 * address + k);
    const bool mini = (w1 >> 30) & 1;
    const uint32_t fetch = mini ? last : ((w0 >> 20) & 0x1F) * 3 + ((w0 >> 25) & 3);
    const uint32_t stride = mini ? last_stride : w2 & 0xFF;
    if (fetch >= 96) return false;
    last = fetch;
    last_stride = stride;
    const uint32_t format = (w1 >> 16) & 0x3F;
    elements.push_back({fetch, stride, (w2 >> 8) & 0x7FFFFF, format, StateVertexBytes(format)});
  }
  return true;
}
// The VS microcode as the commit patches it for the bound declaration (+11812)
// and stream strides (+12520): 82140540 runs on a copy in an own scratch area
// of the system heap, as the own draw does (PatchVertexFetches); the code stays
// unpatched without a declaration or with container flag 0x40.
extern "C" void __imp__sub_82140540(PPCContext& __restrict, uint8_t*);
std::mutex state_patch_mutex;
uint32_t state_patch_area = 0;
constexpr uint32_t kStatePatchBytes = 0x10000;
bool StatePatchedCode(PPCContext& ctx, uint8_t* base, uint32_t dev, const BoundShader& vs,
                      std::vector<uint32_t>& code) {
  code = vs.code;
  const uint32_t decl = Get32(base, dev + 11812), flags = Get32(base, vs.container);
  if (!decl || (flags & 0x40)) return true;
  if (code.size() * 4 > kStatePatchBytes) return false;
  std::lock_guard lock(state_patch_mutex);
  if (!state_patch_area) {
    auto* ts = rex::runtime::ThreadState::Get();
    if (!ts || !(state_patch_area = ts->memory()->SystemHeapAlloc(kStatePatchBytes, 0x1000))) return false;
  }
  for (size_t i = 0; i < code.size(); ++i) Put32(base, state_patch_area + uint32_t(4 * i), code[i]);
  std::array<uint8_t, sizeof(PPCContext)> saved{};
  std::memcpy(saved.data(), &ctx, sizeof(PPCContext));
  ctx.r1.u64 = (ctx.r1.u32 - 1024) & ~15u;  // Below the caller's frame.
  ctx.r3.u64 = vs.object;
  ctx.r4.u64 = state_patch_area;
  ctx.r5.u64 = decl;
  ctx.r6.u64 = dev + 12520;
  ctx.r7.u64 = 0;
  __imp__sub_82140540(ctx, base);
  std::memcpy(&ctx, saved.data(), sizeof(PPCContext));
  for (size_t i = 0; i < code.size(); ++i) code[i] = Get32(base, state_patch_area + uint32_t(4 * i));
  return true;
}
}  // namespace gpu

namespace gpu {
// kind 0 DrawIndexedVertices, 1 DrawVertices, 2 BeginVertices (no streams: the
// title writes its vertices after the entry).
void StateNoteDraw(PPCContext& ctx, uint8_t* base, unsigned kind) {
  if (!State().frames) return;
  const uint32_t dev = ctx.r3.u32;
  std::array<uint64_t, kStateComponents> c{};
  const std::array<uint32_t, 6> args{kind, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, kind == 0 ? ctx.r7.u32 : 0, 0};
  c[0] = StateWords(args.data(), args.size());
  std::vector<uint32_t> vfetch_words, patched, unused_words;
  std::vector<StateElement> elements;
  BoundShader vs, ps;
  const bool vs_read = ReadBoundShader(base, Get32(base, dev + 12688), true, vs);
  std::vector<StateElement> unpatched;
  const bool parsed = vs_read && StateVertexElements(base, vs, vs.code, vfetch_words, unpatched) &&
                      StatePatchedCode(ctx, base, dev, vs, patched) &&
                      StateVertexElements(base, vs, patched, unused_words, elements);
  if (!parsed) elements.clear();
  c[1] = vs_read ? StateShader(vs, vfetch_words) : 0;
  c[2] = ReadBoundShader(base, Get32(base, dev + 12684), false, ps) ? StateShader(ps, {}) : 0;
  c[3] = Fnv64(base + dev + 0x780, 4096);
  c[4] = Fnv64(base + dev + 0x1780, 4096);
  std::array<uint32_t, 192> fetch{};
  for (unsigned i = 0; i < 192; ++i) fetch[i] = Get32(base, dev + 0x480 + 4 * i);
  for (unsigned slot = 0; slot < 32; ++slot) {
    uint32_t* w = &fetch[6 * slot];
    if ((w[0] & 3) == 2) {  // Texture: base and mip addresses.
      w[1] &= 0xFFF;
      w[5] &= 0xFFF;
    } else {
      for (unsigned p = 0; p < 3; ++p)
        if ((w[2 * p] & 3) == 3) w[2 * p] &= 3;  // Vertex: address.
    }
  }
  c[5] = StateWords(fetch.data(), fetch.size());
  std::array<uint32_t, 52> regs{};
  for (unsigned i = 0; i < 19; ++i) regs[i] = Get32(base, dev + 10368 + 4 * i);
  for (unsigned i = 0; i < 21; ++i) regs[19 + i] = Get32(base, dev + 10444 + 4 * i);
  for (unsigned i = 0; i < 12; ++i) regs[40 + i] = Get32(base, dev + 10548 + 4 * i);
  c[6] = StateWords(regs.data(), regs.size());
  // Indices of an indexed draw (as stored, with their endian), and the vertex range.
  int64_t first = 0, last = -1;
  if (kind == 0) {
    const uint32_t ib = Get32(base, dev + 12428);
    c[8] = 4;
    if (ib) {
      const uint32_t common = Get32(base, ib), size = (common >> 31) & 1 ? 4 : 2, endian = (common >> 29) & 3;
      const uint32_t count = std::min<uint32_t>(ctx.r7.u32, 65536);
      std::vector<uint8_t> indices(size_t(count) * size);
      const bool read = ReadPhysical(base, GuestPhysicalAlias(Get32(base, ib + 0x18)) + ctx.r6.u32 * size,
                                     uint32_t(indices.size()), indices.data());
      c[8] = read ? Fnv64(indices.data(), indices.size()) ^ endian : 3;
      if (read && count) {
        uint32_t low = UINT32_MAX, high = 0;
        for (uint32_t i = 0; i < count; ++i) {
          uint32_t v;
          if (size == 4) {
            std::memcpy(&v, &indices[4 * i], 4);
            if (endian == 2) v = std::byteswap(v);
          } else {
            uint16_t h;
            std::memcpy(&h, &indices[2 * i], 2);
            v = endian == 1 ? std::byteswap(h) : h;
          }
          low = std::min(low, v);
          high = std::max(high, v);
        }
        first = int64_t(low) + int32_t(ctx.r5.u32);
        last = int64_t(high) + int32_t(ctx.r5.u32);
      }
    }
  } else if (kind == 1 && ctx.r6.u32) {
    first = ctx.r5.u32;
    last = int64_t(ctx.r5.u32) + ctx.r6.u32 - 1;
  }
  // Vertex streams: only the bytes of the elements the VS fetches, for the
  // vertices the draw uses (padding between elements is not an input).
  c[7] = parsed ? 0x9E3779B97F4A7C15ull : 1;
  std::vector<uint32_t> fetches;
  for (const auto& e : elements)
    if (std::find(fetches.begin(), fetches.end(), e.fetch) == fetches.end()) fetches.push_back(e.fetch);
  const int64_t frame = state_draw_frame.load();
  const bool dump = State().dump_first >= 1 && frame >= State().dump_first && frame <= State().dump_last;
  const bool raster = State().raster_first >= 1 && frame >= State().raster_first && frame <= State().raster_last;
  std::string extents;  // Float elements: fetch, offset, format, vertices, min and max of the first two components.
  std::vector<uint8_t> raw;
  if (dump) DumpWords(raw, {kind, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, kind == 0 ? ctx.r7.u32 : 0u, uint32_t(ctx.lr),
                            kind != 2 ? uint32_t(fetches.size()) : 0u});
  if (kind != 2) {
    std::vector<uint8_t> bytes, used;
    for (uint32_t f : fetches) {
      const uint32_t at = dev + 0x480 + 24 * (f / 3) + 8 * (f % 3);
      const uint32_t d0 = Get32(base, at), d1 = Get32(base, at + 4), size = ((d1 >> 2) & 0xFFFFFF) * 4;
      uint32_t stride = 0;
      for (const auto& e : elements)
        if (e.fetch == f) { stride = e.stride * 4; break; }
      const std::array<uint32_t, 4> head{f, size, d1 & 3, stride};
      c[7] = (c[7] ^ StateWords(head.data(), head.size())) * 0x100000001B3ull;
      const uint64_t begin = first >= 0 ? uint64_t(first) * stride : 0;
      const uint64_t end = last >= first && first >= 0 ? uint64_t(last + 1) * stride : 0;
      const bool in_range = stride && end > begin && end <= size && end - begin <= 4u * 1024 * 1024;
      bytes.resize(in_range ? size_t(end - begin) : 0);
      const bool read = in_range && (d0 & 3) == 3 &&
                        ReadPhysical(base, (d0 & 0x1FFFFFFCu) + uint32_t(begin), uint32_t(end - begin), bytes.data());
      used.clear();
      if (read)
        for (size_t v = 0; v + stride <= bytes.size(); v += stride)
          for (const auto& e : elements)
            if (e.fetch == f) {
              const size_t from = v + 4 * size_t(e.offset), n = e.bytes ? e.bytes : 16;
              if (from + n <= bytes.size()) used.insert(used.end(), bytes.begin() + from, bytes.begin() + from + n);
            }
      c[7] = (c[7] ^ (read ? Fnv64(used.data(), used.size()) : 2)) * 0x100000001B3ull;
      if (raster && read)
        for (const auto& e : elements) {
          if (e.fetch != f || (e.format != 37 && e.format != 38 && e.format != 57)) continue;
          float low[2] = {0, 0}, high[2] = {0, 0};
          uint32_t vertices = 0;
          for (size_t v = 0; v + stride <= bytes.size(); v += stride) {
            const size_t from = v + 4 * size_t(e.offset);
            if (from + 8 > bytes.size()) break;
            for (unsigned k = 0; k < 2; ++k) {
              uint32_t word;
              std::memcpy(&word, &bytes[from + 4 * k], 4);
              if ((d1 & 3) == 2) word = std::byteswap(word);
              float value;
              std::memcpy(&value, &word, 4);
              if (!vertices || value < low[k]) low[k] = value;
              if (!vertices || value > high[k]) high[k] = value;
            }
            ++vertices;
          }
          extents += fmt::format("{}{{\"fetch\":{},\"offset\":{},\"format\":{},\"endian\":{},\"vertices\":{},"
                                 "\"min\":[{},{}],\"max\":[{},{}]}}", extents.empty() ? "" : ",", f, e.offset,
                                 e.format, d1 & 3, vertices, JsonFloat(low[0]), JsonFloat(low[1]), JsonFloat(high[0]),
                                 JsonFloat(high[1]));
        }
      if (dump) {
        const uint32_t kept = read ? uint32_t(std::min<size_t>(bytes.size(), 16 * 1024)) : 0;
        DumpWords(raw, {f, size, d1 & 3, kept});
        raw.insert(raw.end(), bytes.begin(), bytes.begin() + kept);
      }
    }
  }
  if (dump) raw.insert(raw.end(), base + dev + 0x780, base + dev + 0x780 + 4096);
  std::string raster_record;
  if (raster) {
    std::array<uint32_t, 8> r2300{};
    for (unsigned i = 0; i < 8; ++i) r2300[i] = Get32(base, dev + 10680 + 4 * i);
    std::array<uint32_t, 16> c0_c3{};
    for (unsigned i = 0; i < 16; ++i) c0_c3[i] = Get32(base, dev + 0x780 + 4 * i);
    raster_record = fmt::format(
        "\"kind\":{},\"args\":[{},{},{},{}],\"lr\":{},\"color\":{},\"depth\":{},\"vs\":\"{:016x}\",\"ps\":\"{:016x}\","
        "\"r2000\":[{}],\"r2100\":[{}],\"r2200\":[{}],\"r2300\":[{}],\"c0_c3\":[{}],\"extents\":[{}]}}", kind,
        ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, kind == 0 ? ctx.r7.u32 : 0u, uint32_t(ctx.lr), Get32(base, dev + 12432),
        Get32(base, dev + 12448), vs_read ? vs.virtual_hash : 0, ps.object ? ps.virtual_hash : 0,
        fmt::join(regs.begin(), regs.begin() + 19, ","), fmt::join(regs.begin() + 19, regs.begin() + 40, ","),
        fmt::join(regs.begin() + 40, regs.end(), ","), fmt::join(r2300, ","), fmt::join(c0_c3, ","), extents);
  }
  const bool candidate = kind == 0 && StartEntryCandidate(base, dev, true, ctx.lr, ctx.r4.u32, ctx.r7.u32);
  bool red = false;
  if (candidate) {
    std::array<uint32_t, 4> c40{};
    for (unsigned i = 0; i < 4; ++i) c40[i] = Get32(base, dev + 0x780 + 16 * 40 + 4 * i);
    red = StartRed(c40);
  }
  std::lock_guard lock(state_mutex);
  auto& f = state_frame;
  for (uint64_t v : c)
    for (unsigned b = 0; b < 8; ++b) f.digest = (f.digest ^ ((v >> (8 * b)) & 0xFF)) * 0x100000001B3ull;
  if (raster)
    state_raster += fmt::format("{}{{\"draw\":{},\"candidate\":{},\"red\":{},{}", state_raster.empty() ? "" : ",\n",
                                f.draws, candidate, candidate && red, raster_record);
  ++f.draws;
  f.red += candidate && red;
  f.white += candidate && !red;
  f.components.push_back(c);
  if (dump) state_dump.insert(state_dump.end(), raw.begin(), raw.end());
}

// Frames of the window (Swap thread).
struct StateRecord {
  uint32_t swaps, vblank, flips;
  uint64_t clock, clock_calls;
  int64_t qpc;
  uint32_t draws, red, white;
  uint64_t digest;
};
std::vector<StateRecord> state_records;
std::vector<std::vector<std::array<uint64_t, kStateComponents>>> state_components;
std::vector<std::string> state_captures;
bool state_window = false, state_written = false;
uint64_t state_clock_calls_seen = 0;

bool StateWriteFile(const std::string& name, const void* data, size_t size, std::string& fields) {
  const char* dir = std::getenv("REX_NATIVE_FRAME_DIR");
  if (!dir || !*dir) { fields = fmt::format("\"file\":null,\"sha256\":null"); return false; }
  std::FILE* out = std::fopen(fmt::format("{}/{}", dir, name).c_str(), "wb");
  const bool written = out && std::fwrite(data, 1, size, out) == size;
  if (out) std::fclose(out);
  fields = fmt::format("\"file\":\"{}\",\"sha256\":\"{}\"", name,
                       written ? Sha256Hex(data, size) : std::string("write-failed"));
  return written;
}
std::string StateFramesFile(bool own) {
  const auto& cfg = State();
  std::string text = fmt::format(
      "{{\"schema\":\"native-5-0-state-frames-v1\",\"mode\":\"{}\",\"clock\":\"{}\",\"clock_counter\":{},"
      "\"clock_step\":{},\"clock_switch\":{{\"on\":{},\"q0\":{},\"v0\":{}}},\"requested\":{},\"components_frames\":{},"
      "\"capture\":[{},{}],\"columns\":\"swaps16544,vblank16532,flips16552,clock,clock_calls,qpc,draws,red,white,"
      "digest\",\"component_names\":[\"args\",\"vs\",\"ps\",\"vsc\",\"psc\",\"fetch\",\"regs\",\"vb\",\"ib\"],"
      "\"frames\":[", own ? "own" : "reference", StateClockName(cfg.clock),
      cfg.clock ? StateClockCounter() : 0, cfg.clock ? StateClockStep() : 0, state_clock_on.load(), state_clock_q0,
      state_clock_v0.load(), cfg.frames, cfg.components, cfg.capture_first, cfg.capture_last);
  for (size_t i = 0; i < state_records.size(); ++i) {
    const auto& r = state_records[i];
    text += fmt::format("{}\n[{},{},{},{},{},{},{},{},{},\"{:016x}\"]", i ? "," : "", r.swaps, r.vblank, r.flips,
                        r.clock, r.clock_calls, r.qpc, r.draws, r.red, r.white, r.digest);
  }
  text += "],\"components\":[";
  for (size_t i = 0; i < state_components.size(); ++i) {
    text += i ? ",\n[" : "\n[";
    for (size_t d = 0; d < state_components[i].size(); ++d) {
      text += d ? ",[" : "[";
      for (size_t k = 0; k < kStateComponents; ++k)
        text += fmt::format("{}\"{:016x}\"", k ? "," : "", state_components[i][d][k]);
      text += "]";
    }
    text += "]";
  }
  text += "],\"captures\":[";
  for (size_t i = 0; i < state_captures.size(); ++i) text += (i ? ",\n" : "\n") + state_captures[i];
  text += "],\"clock_census_columns\":\"guest_thread,lr,caller_lr,calls\",\"clock_census\":[";
  {
    state_census_on.store(false);
    std::lock_guard lock(state_census_mutex);
    std::vector<std::pair<uint64_t, std::array<uint32_t, 3>>> rows;
    for (const auto& [key, calls] : state_census) rows.emplace_back(calls, key);
    std::sort(rows.begin(), rows.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    for (size_t i = 0; i < rows.size(); ++i)
      text += fmt::format("{}\n[{},{},{},{}]", i ? "," : "", rows[i].second[0], rows[i].second[1], rows[i].second[2],
                          rows[i].first);
  }
  text += "]}\n";
  std::string fields;
  StateWriteFile("state-frames.json", text.data(), text.size(), fields);
  return fields;
}

// Own capture: the own texture of the front buffer at the Swap entry.
std::string StateCaptureOwn(uint32_t front, uint64_t k) {
  std::vector<uint8_t> rgba;
  uint32_t width = 0, height = 0;
  std::string evidence;
  auto* dispatcher = host::ActiveDispatcher();
  const bool read = dispatcher && dispatcher->Run([&](Engine& engine, std::string& out) {
    return engine.ReadTexture(front, rgba, width, height, out); }, evidence);
  if (!read) return fmt::format("{{\"k\":{},\"read\":false,\"engine\":{}}}", k, evidence.empty() ? "null" : evidence);
  std::string fields;
  StateWriteFile(fmt::format("state-k{:04}-own-{}x{}.rgba", k, width, height), rgba.data(), rgba.size(), fields);
  return fmt::format("{{\"k\":{},\"read\":true,\"format\":\"rgba8\",\"width\":{},\"height\":{},{}}}", k, width, height,
                     fields);
}

// Reference capture: after the original Swap, BlockOnFence(dev, +14924, 3) on
// the frame fence, then the guest bytes of the front buffer (pitch x height
// aligned to 32, 4 bytes per texel), as the fetch constant at header +28 gives.
extern "C" void __imp__sub_8212F708(PPCContext& __restrict, uint8_t*);
std::string StateCaptureReference(PPCContext& ctx, uint8_t* base, uint32_t dev, const std::array<uint32_t, 6>& f,
                                  uint64_t k) {
  const uint32_t fence = Get32(base, dev + 14924);
  std::array<uint8_t, sizeof(PPCContext)> saved{};
  std::memcpy(saved.data(), &ctx, sizeof(PPCContext));
  ctx.r1.u64 = (ctx.r1.u32 - 1024) & ~15u;
  ctx.r3.u64 = dev;
  ctx.r4.u64 = fence;
  ctx.r5.u64 = 3;
  __imp__sub_8212F708(ctx, base);
  std::memcpy(&ctx, saved.data(), sizeof(PPCContext));
  const uint32_t pitch = ((f[0] >> 22) & 0x1FF) * 32, width = (f[2] & 0x1FFF) + 1;
  // The base is a virtual address; 0xE0000000 and above alias physical memory + 0x1000.
  const uint32_t height = ((f[2] >> 13) & 0x1FFF) + 1, physical = GuestPhysicalAlias(f[1] & 0xFFFFF000u);
  const uint32_t size = pitch * ((height + 31) & ~31u) * 4;
  std::vector<uint8_t> bytes(size);
  const bool read = size && size <= 64u * 1024 * 1024 && ReadPhysical(base, physical, size, bytes.data());
  std::string fields = "\"file\":null,\"sha256\":null";
  if (read) StateWriteFile(fmt::format("state-k{:04}-reference-guest.bin", k), bytes.data(), bytes.size(), fields);
  return fmt::format("{{\"k\":{},\"read\":{},\"fence14924\":{},\"fetch\":[{}],\"pitch\":{},\"width\":{},"
                     "\"height\":{},\"physical\":{},\"bytes\":{},{}}}", k, read, fence, fmt::join(f, ","), pitch,
                     width, height, physical, size, fields);
}

// At each Swap entry, both modes: closes the frame that just ended. Returns the
// window index of a reference frame to capture after the original Swap, or -1.
int64_t StateAtSwap(PPCContext& ctx, uint8_t* base, uint32_t dev, bool own, uint32_t front) {
  const auto& cfg = State();
  if (!cfg.frames && !cfg.clock) return -1;
  if (!cfg.valid) {
    if (own) StopInOrder(ctx, "state-variables-invalid", 88);
    std::_Exit(88);
  }
  // No vblank is emulated in the own mode, so +16532 no longer counts.
  if (own && cfg.clock == 1) StopInOrder(ctx, "state-clock-vblank-uncovered", 88);
  if (cfg.clock > 0 && !state_clock_device.load()) {
    state_clock_device.store(dev, std::memory_order_release);
    if (cfg.clock == 3) {  // flip-frozen runs from the first Swap.
      state_clock_q0 = kStateFrozenQ0;
      state_clock_on.store(true, std::memory_order_release);
    }
  }
  if (!cfg.frames || state_written) return -1;
  StateFrame f;
  std::vector<uint8_t> dumped;
  std::string rastered;
  {
    std::lock_guard lock(state_mutex);
    std::swap(f, state_frame);
    std::swap(dumped, state_dump);
    std::swap(rastered, state_raster);
  }
  if ((f.red || f.white) && !state_window) {
    state_window = true;
    state_census_on.store(true);
    if (cfg.clock == 4) {  // start-frozen runs from this Swap.
      state_clock_q0 = kStateFrozenQ0;
      state_clock_v0.store(Get32(base, dev + 16552));
      state_clock_on.store(true, std::memory_order_release);
    }
  }
  if (!state_window) return -1;
  state_draw_frame.store(int64_t(state_records.size()) + 1);  // The frame drawn next.
  if (!dumped.empty()) {
    std::string fields;
    StateWriteFile(fmt::format("state-k{:04}-draws.bin", state_records.size()), dumped.data(), dumped.size(), fields);
    state_captures.push_back(fmt::format("{{\"k\":{},\"dump\":true,\"draws\":{},\"bytes\":{},{}}}",
                                         state_records.size(), f.draws, dumped.size(), fields));
  }
  if (!rastered.empty()) {
    const std::string text = fmt::format("{{\"schema\":\"native-5-1-state-raster-v1\",\"mode\":\"{}\",\"k\":{},"
                                         "\"draws\":{},\"records\":[\n{}]}}\n", own ? "own" : "reference",
                                         state_records.size(), f.draws, rastered);
    std::string fields;
    StateWriteFile(fmt::format("state-k{:04}-raster.json", state_records.size()), text.data(), text.size(), fields);
    state_captures.push_back(fmt::format("{{\"k\":{},\"raster\":true,\"draws\":{},{}}}", state_records.size(),
                                         f.draws, fields));
  }
  const int64_t now = platform::Ticks();
  const uint64_t calls = state_clock_calls.load();
  state_records.push_back({Get32(base, dev + 16544), Get32(base, dev + 16532), Get32(base, dev + 16552),
                           state_clock_last.load(), calls - state_clock_calls_seen, now, f.draws, f.red,
                           f.white, f.digest});
  state_clock_calls_seen = calls;
  const int64_t k = int64_t(state_records.size()) - 1;
  if (k < cfg.components) state_components.push_back(std::move(f.components));
  int64_t capture = -1;
  if (k >= cfg.capture_first && k <= cfg.capture_last && cfg.capture_first >= 0) {
    if (own) state_captures.push_back(StateCaptureOwn(front, uint64_t(k)));
    else capture = k;
  }
  if (int64_t(state_records.size()) < cfg.frames) return capture;
  if (!own && capture >= 0) return capture;  // The file is written after this capture.
  state_written = true;
  const std::string file = StateFramesFile(own);
  if (own) {
    host::Record("native-d3d-state", file + fmt::format(",\"frames\":{},\"captures\":{}",
                                                                   state_records.size(), state_captures.size()));
    StopInOrder(ctx, "native-d3d-state-complete", 86);
  }
  std::fflush(nullptr);
  std::_Exit(86);  // Reference mode: the window ends with the file.
}
// Reference mode, after the original Swap of a frame to capture.
void StateAfterSwapReference(PPCContext& ctx, uint8_t* base, uint32_t dev, const std::array<uint32_t, 6>& f,
                             int64_t k) {
  state_captures.push_back(StateCaptureReference(ctx, base, dev, f, uint64_t(k)));
  if (int64_t(state_records.size()) < State().frames) return;
  state_written = true;
  StateFramesFile(false);
  std::fflush(nullptr);
  std::_Exit(86);
}
}  // namespace gpu
