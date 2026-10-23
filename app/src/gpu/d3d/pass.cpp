// Pass layer of the D3D boundary: D3DDevice_BeginTiling and
// D3DDevice_ClearF as own operations, D3DDevice_Resolve and EndTiling.
//
// BeginTiling keeps every CPU store of the original that code still running
// original reads (SetRenderTarget, SetDepthStencilSurface, the viewport
// helper): tile layout, clear parameters, copies of the bound slots and flag
// bits. Its EDRAM tiling infrastructure (per-tile command buffers 13232..13352,
// inline events, ring kick) is not reproduced: the native pass has no tiles.
#include "gpu/d3d/pass.h"
#include "gpu/diagnostics/diagnostics.h"
#include "gpu/diagnostics/census.h"
#include "gpu/d3d/hooks.h"
#include "gpu/d3d/device.h"
#include "gpu/d3d/resources.h"
#include "gpu/d3d/draw.h"
#include "gpu/diagnostics/frame_trace.h"
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
// Two different bound headers (render targets 0..3 and depth) with the same
// EDRAM base (RB_COLOR_INFO / RB_DEPTH_INFO at header +0x1C, 12 bits in tiles)
// alias on the console. The own surfaces are per header and would not share
// the content, so the pass or the draw stops in a guard. START: color at 0,
// depth at 0x2D0 (1X) and 0x400 (4X).
bool SharedEdramBase(const std::array<uint32_t, 5>& headers, const std::array<uint32_t, 5>& edram_bases) {
  for (unsigned i = 0; i < 5; ++i) {
    if (!headers[i]) continue;
    for (unsigned j = 0; j < i; ++j)
      if (headers[j] && headers[j] != headers[i] && edram_bases[j] == edram_bases[i]) return true;
  }
  return false;
}
bool SharedEdramBase(uint8_t* base, uint32_t dev) {
  std::array<uint32_t, 5> headers{}, bases{};
  for (unsigned i = 0; i < 5; ++i) {
    headers[i] = Get32(base, dev + 12432 + 4 * i);
    bases[i] = headers[i] ? Get32(base, headers[i] + 0x1C) & 0xFFF : 0;
  }
  return SharedEdramBase(headers, bases);
}

std::array<float, 4> GuestColor(uint8_t* base, uint32_t pointer) {
  std::array<float, 4> color{};  // Null pointer: the original uses zero.
  if (pointer)
    for (unsigned i = 0; i < 4; ++i) color[i] = GetFloat(base, pointer + 4 * i);
  return color;
}

// Tiling backings. On the console a tiled pass renders each tile into the
// EDRAM-sized surface bound by the title (1280 x 256 here) and resolves it at
// the tile offset. The native pass has no tiles: between BeginTiling and
// EndTiling every bound surface is replaced by an own full-size backing
// (tile extent max_x x max_y, same format and samples). Key = header | 1.
std::mutex backing_mutex;
std::unordered_map<uint32_t, uint32_t> backings;  // bound header -> backing key
uint32_t Backing(uint32_t key) {
  std::lock_guard lock(backing_mutex);
  auto it = backings.find(key);
  return it == backings.end() ? key : it->second;
}
void BeginBackings(PPCContext& ctx, uint8_t* base, uint32_t dev, uint32_t width, uint32_t height) {
  // Not covered: without the census, each surface still gets its own backing (the aliasing is not kept).
  if (SharedEdramBase(base, dev)) UncoveredOperation(ctx, "BeginTiling-uncovered-EDRAM.base_shared");
  for (uint32_t slot : {12432u, 12436u, 12440u, 12444u, 12448u}) {
    const uint32_t key = Get32(base, dev + slot);
    if (!key) continue;
    const uint32_t size_bits = Get32(base, key + 0x24), format = Get32(base, key + 0x28);
    SurfaceDesc desc{key | 1, width, height, format, resources.Samples(key)};
    if (!desc.samples) StopInOrder(ctx, "BeginTiling-surface-unregistered", 88);
    desc.format = slot == 12448 ? SurfaceFormat::DepthStencil : SurfaceFormat::Color8888;
    // The backing keeps the format the D330 covered. A surface left
    // without an own surface (format outside the covered one) stops here with
    // its field, instead of getting an RGBA8 backing.
    ResourceRegistry::Entry entry{};
    if (resources.Find(key, entry) && !entry.host_surface) {
      const char* field = slot == 12448 ? (DepthFormatCovered(format) ? "surface-kind" : "D3DFORMAT.depth_format")
                                        : UncoveredColorField(format, Get32(base, key + 0x1C));
      // The surface keeps no backing; its draws meet their own guard.
      UncoveredOperation(ctx, fmt::format("BeginTiling-uncovered-{}", field ? field : "surface-kind"));
      continue;
    }
    if ((size_bits >> 18) + 1 >= width && ((size_bits >> 3) & 0x7FFF) + 1 >= height) continue;  // Already full size.
    // Posted, as the clears, except the first 16 backings whose record keeps
    // the engine evidence: the owner runs in order, so the pass's draws and
    // resolves come after the backing, and a failure latches and stops at the
    // next fence. A wait here held the title behind everything queued before
    // it (at the entry of the village, ~300 texture creations: 118 ms per
    // frame).
    std::string evidence = "\"posted\"";
    static std::atomic<uint64_t> backing_evidence{0};
    const bool evidence_backing = Diagnostics() && backing_evidence.fetch_add(1) < 16;
    auto* dispatcher = host::ActiveDispatcher();
    const auto ensure = [desc](Engine& engine, std::string& out) { return engine.EnsureSurface(desc, out); };
    if (!dispatcher || !(evidence_backing ? dispatcher->Run(ensure, evidence) : dispatcher->Post(ensure))) {
      RecordPostedFailure("BeginTiling-backing");
      StopInOrder(ctx, "BeginTiling-backing-unproved", 88);
    }
    {
      std::lock_guard lock(backing_mutex);
      backings[key] = desc.key;
    }
    static std::atomic<uint64_t> backing_records{0};  // Capped in the measurement.
    if (Diagnostics() && (!profile::QuietRecords() || backing_records.fetch_add(1) < profile::kQuietRecords))
      host::Record("native-d3d-tiling-backing", fmt::format(
        "\"bound\":{},\"backing\":{},\"size\":[{},{}],\"samples\":{},\"engine\":{}", key, desc.key, width,
        height, desc.samples, evidence));
  }
}
void EndBackings() {
  std::lock_guard lock(backing_mutex);
  backings.clear();
}

// Own clear of the bound surfaces. Flags follow D3DCLEAR: 0xF targets,
// 0x10 Z, 0x20 stencil. Without a depth binding Z/stencil are dropped, as in
// the original (82130988).
std::atomic<uint32_t> pass_records{0};
void OwnClear(PPCContext& ctx, uint8_t* base, uint32_t dev, uint32_t flags, const std::array<int32_t, 4>& rect,
              const std::array<float, 4>& color, float z, uint32_t stencil, const char* source) {
  profile::Timer profile(profile::kClear);
  ClearRequest request{};
  for (unsigned i = 0; i < 4; ++i)
    request.color_keys[i] = (flags & (1u << i)) ? Backing(Get32(base, dev + 12432 + 4 * i)) : 0;
  request.depth_key = Backing(Get32(base, dev + 12448));
  request.clear_color = (flags & 0xF) != 0;
  request.clear_depth = request.depth_key && (flags & 0x10);
  request.clear_stencil = request.depth_key && (flags & 0x20);
  request.color = color;
  request.depth = z;
  request.stencil = uint8_t(stencil);
  request.left = rect[0]; request.top = rect[1]; request.right = rect[2]; request.bottom = rect[3];
  std::string evidence;
  uint64_t fence = 0;
  auto* dispatcher = host::ActiveDispatcher();
  // Posted, except the first 16 passes whose record keeps the engine evidence.
  const bool evidence_pass = Diagnostics() && pass_records.fetch_add(1) < 16;
  if (!dispatcher || dispatcher->PostedFailure(evidence)) {
    RecordPostedFailure(source);
    StopInOrder(ctx, fmt::format("{}-own-clear-unproved", source), 88);
  }
  const bool done = evidence_pass ?
      dispatcher->Run([&request, &fence](Engine& engine, std::string& out) {
        return engine.ClearSurfaces(request, fence, out); }, evidence) :
      dispatcher->Post([request](Engine& engine, std::string& out) {
        uint64_t posted_fence = 0;
        return engine.ClearSurfaces(request, posted_fence, out); });
  if (!done) {
    RecordPostedFailure(source);
    StopInOrder(ctx, fmt::format("{}-own-clear-unproved", source), 88);
  }
  if (device_owner) device_owner->NoteSubmission();
  if (FrameTraceActive())
    TraceFrameOp("clear", fmt::format(
        "\"source\":\"{}\",\"flags\":{},\"targets\":[{},{},{},{}],\"depth\":{},\"color\":[{},{},{},{}],\"z\":{},"
        "\"stencil\":{},\"rect\":[{},{},{},{}],\"lr\":{}", source, flags, request.color_keys[0], request.color_keys[1],
        request.color_keys[2], request.color_keys[3], request.depth_key, color[0], color[1], color[2], color[3], z,
        stencil, rect[0], rect[1], rect[2], rect[3], ctx.lr));
  if (evidence_pass) {
    auto* ts = rex::runtime::ThreadState::Get();
    host::Record("native-d3d-clear", fmt::format(
        "\"source\":\"{}\",\"flags\":{},\"lr\":{},\"guest_thread\":{},\"targets\":[{},{},{},{}],\"depth\":{},"
        "\"engine\":{}", source, flags, ctx.lr, ts ? ts->thread_id() : 0, request.color_keys[0],
        request.color_keys[1], request.color_keys[2], request.color_keys[3], request.depth_key, evidence));
  }
}
}  // namespace gpu

// D3DDevice_ClearF(device, Flags, pRect, pColor, Z, Stencil): own clear. The
// original's commit of dirty state is not reproduced; the own draw reads the
// device state directly.
extern "C" void __imp__sub_82131018(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_82131018) {
  if (!gpu::Active()) { __imp__sub_82131018(ctx, base); return; }
  HookScope scope("ClearF");
  if (!device_owner || !device_owner->live() || ctx.r3.u32 != device_owner->device()) StopInOrder(ctx, "ClearF-device", 88);
  TakePendingDraw(&ctx, base, "ClearF");
  const uint32_t dev = ctx.r3.u32;
  std::array<int32_t, 4> rect{};
  if (ctx.r5.u32)
    for (unsigned i = 0; i < 4; ++i) rect[i] = int32_t(Get32(base, ctx.r5.u32 + 4 * i));
  OwnClear(ctx, base, dev, ctx.r4.u32, rect, GuestColor(base, ctx.r6.u32), float(ctx.f1.f64), ctx.r8.u32, "ClearF");
}

// D3DDevice_BeginTiling(device, Flags, Count, pTileRects, pClearColor, Z, -, Stencil),
// specialized by LTCG to Count = 3 and the tile table at 82081798.
extern "C" void __imp__sub_8212AC00(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8212AB00(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82128550(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8213A7E0(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_8213A7E0) {
  if (!gpu::Active()) { __imp__sub_8213A7E0(ctx, base); return; }
  HookScope scope("BeginTiling");
  if (!device_owner || !device_owner->live() || ctx.r3.u32 != device_owner->device() ||
      ctx.r5.u32 != 3 || ctx.r6.u32 != 0x82081798) StopInOrder(ctx, "BeginTiling-signature", 88);
  TakePendingDraw(&ctx, base, "BeginTiling");
  const uint32_t dev = ctx.r3.u32, flags = ctx.r4.u32, color_ptr = ctx.r7.u32, stencil = ctx.r9.u32;
  const double z = ctx.f1.f64;
  constexpr uint32_t kTiles = 0x82081798, kViewport = 0x8208B398, kScissor = 0x8208B3B0;
  Put32(base, dev + 12740, 3);
  uint32_t max_x = 0, max_y = 0;
  for (uint32_t i = 0; i < 3; ++i) {
    const uint32_t x1 = Get32(base, kTiles + 16 * i), y1 = Get32(base, kTiles + 16 * i + 4);
    const uint32_t x2 = Get32(base, kTiles + 16 * i + 8), y2 = Get32(base, kTiles + 16 * i + 12);
    max_x = std::max(max_x, x2);
    max_y = std::max(max_y, y2);
    Put32(base, dev + 12744 + 16 * i, x1); Put32(base, dev + 12748 + 16 * i, y1);
    Put32(base, dev + 12752 + 16 * i, x2); Put32(base, dev + 12756 + 16 * i, y2);
    Put32(base, dev + 12984 + 8 * i, x1 & ~31u); Put32(base, dev + 12988 + 8 * i, y1 & ~31u);
  }
  if (flags & 2) {  // Predicated tiling with a fixed EDRAM layout.
    uint8_t b = Get8(base, dev + 10943) | 0x20;
    if (flags & 4) b |= 0x10;
    Put8(base, dev + 10943, b);
    const uint32_t mode = Get16(base, dev + 10368) & 3;
    const uint32_t row = mode == 0 ? 16 : 8, column = mode <= 1 ? 32 : 16, pitch = mode <= 1 ? 80 : 40;
    const uint32_t width = (column + max_x - 1) & ~(column - 1), per_row = width / column;
    for (uint32_t i = 0; i < 3; ++i)
      Put32(base, dev + 13104 + 4 * i, Get32(base, dev + 12988 + 8 * i) / row * per_row +
                                       Get32(base, dev + 12984 + 8 * i) / column);
    Put32(base, dev + 13164, width);
    Put32(base, dev + 10368, (Get32(base, dev + 10368) & 0x0003FFFF) | (width << 18));
    Or64(base, dev + 16, 0x100);
    Put32(base, dev + 13168, (pitch + max_x - 1) / pitch * pitch);
    if ((Get8(base, dev + 10943) & 0x20) || (Get32(base, dev + 11844) & 0x000E0000) != 0x40000)
      Or64(base, dev + 40, 0x100);
    else
      And64(base, dev + 40, ~uint64_t(0x100));
  }
  Put32(base, dev + 13172, max_x);
  Put32(base, dev + 13176, max_y);
  Put32(base, dev + 13180, flags);
  BeginBackings(ctx, base, dev, max_x, max_y);
  const uint32_t frame = (ctx.r1.u32 - 512) & ~15u;  // Below the caller's frame.
  if (!(flags & 1) && !(Get8(base, dev + 10943) & 0x20)) {
    uint32_t tile_w = 0, tile_h = 0;
    for (uint32_t i = 0; i < 3; ++i) {
      tile_w = std::max(tile_w, Get32(base, dev + 12752 + 16 * i) - Get32(base, dev + 12984 + 8 * i));
      tile_h = std::max(tile_h, Get32(base, dev + 12756 + 16 * i) - Get32(base, dev + 12988 + 8 * i));
    }
    std::array<double, 6> viewport{};
    for (unsigned i = 0; i < 6; ++i) viewport[i] = GetFloat(base, dev + 12640 + 4 * i);
    const uint32_t viewport_word = Get32(base, dev + 12664);
    std::memcpy(base + frame + 96, base + dev + 12668, 16);  // Saved scissor.
    CallGuest(ctx, base, __imp__sub_8212AC00, frame, dev, kViewport);
    CallGuest(ctx, base, __imp__sub_8212AB00, frame, dev, kScissor);
    // The original clears one tile of EDRAM (tile_w x tile_h); the native pass
    // clears its whole backing.
    (void)tile_w; (void)tile_h;
    OwnClear(ctx, base, dev, 0x3F, {0, 0, 0, 0}, GuestColor(base, color_ptr), float(z), stencil, "BeginTiling");
    CallGuest(ctx, base, __imp__sub_82128550, frame, dev, 0, &viewport, viewport_word);
    CallGuest(ctx, base, __imp__sub_8212AB00, frame, dev, frame + 96);
  }
  const auto color = GuestColor(base, color_ptr);
  for (unsigned i = 0; i < 4; ++i) PutFloat(base, dev + 13184 + 4 * i, color[i]);
  PutFloat(base, dev + 13200, float(z));
  Put32(base, dev + 13204, stencil);
  const uint32_t t0 = Get32(base, dev + 12432);
  Put32(base, dev + 12720, t0);
  Put32(base, dev + 12716, t0 ? t0 : Get32(base, dev + 12448));
  Put32(base, dev + 12724, Get32(base, dev + 12436));
  Put32(base, dev + 12728, Get32(base, dev + 12440));
  Put32(base, dev + 12732, Get32(base, dev + 12444));
  Put32(base, dev + 12736, Get32(base, dev + 12448));
  Put32(base, dev + 12712, 2);
  Put32(base, dev + 12708, 0);
  const uint8_t b = Get8(base, dev + 10940) | 0x20;
  Put8(base, dev + 10940, b);
  uint32_t bit = 0;
  if (!(b & 0x08) && !(b & 0x04) && !Get8(base, dev + 12179))
    bit = (Get8(base, dev + 10940) & 0x10) ? 1 : ((Get8(base, dev + 10940) >> 5) & 1);
  Put32(base, dev + 12704, 0);
  const uint8_t flags10940 = uint8_t((Get8(base, dev + 10940) & 0xFE) | (bit ? 1 : 0));
  Put32(base, dev + 10932, 0);
  Put32(base, dev + 10936, 0);
  Put32(base, dev + 12700, 0xFFFFFFFF);
  Put8(base, dev + 10940, flags10940);
  CallGuest(ctx, base, __imp__sub_8212AC00, frame, dev, kViewport);
  for (uint32_t at = 0; at < 40; at += 4) Put32(base, dev + at, 0xFFFFFFFF);  // 821280B0(device, 1)
  if (FrameTraceActive())
    TraceFrameOp("begin-tiling", fmt::format(
        "\"flags\":{},\"max\":[{},{}],\"targets\":[{},{},{},{}],\"depth\":{},\"z\":{},\"lr\":{}", flags, max_x, max_y,
        Get32(base, dev + 12432), Get32(base, dev + 12436), Get32(base, dev + 12440), Get32(base, dev + 12444),
        Get32(base, dev + 12448), z, ctx.lr));
  if (Diagnostics() && pass_records.fetch_add(1) < 16)
    gpu::host::Record("native-d3d-begin-tiling", fmt::format(
        "\"flags\":{},\"max\":[{},{}],\"color_ptr\":{},\"z\":{},\"stencil\":{},\"lr\":{},\"tiling_infrastructure\":false",
        flags, max_x, max_y, color_ptr, z, stencil, ctx.lr));
}

// D3DDevice_Resolve(device, Flags, pSourceRect, pDestTexture, pDestPoint, DestLevel,
// DestSlice, pClearColor, Z, Stencil, pParameters). Source: Flags & 7 (0..3
// render target, 4 depth). 0x100 / 0x200 clear the source afterwards.
// Destination size and format come from the texture fetch constant (+0x1C).
// The register shadows the original writes (10776..10820) feed only its own
// serialized commit, which the D3D boundary does not run.
namespace gpu {
std::atomic<uint32_t> resolve_select_records{0};  // The first color resolves of one sample or pair.
// One own resolve (D3DDevice_Resolve semantics) plus the optional clear of the
// source afterwards. Source: flags & 7 (0..3 render target, 4 depth); 0x100 /
// 0x200 clear the render target / depth-stencil after the copy.
void OwnResolve(PPCContext& ctx, uint8_t* base, uint32_t dev, uint32_t flags, uint32_t rect, uint32_t dest,
                uint32_t point, uint32_t color, float z, uint32_t stencil, const char* source) {
  const uint32_t src_index = flags & 7;
  if (src_index > 4) {  // No source, no resolve.
    UncoveredOperation(ctx, fmt::format("{}-source-uncovered", source));
    return;
  }
  // The flags the original reads are the source (bits 0..2), the sample
  // select (4..6) and the two clears (8, 9). DestLevel and DestSliceOrFace are
  // not read by the original (LTCG: every caller passes 0), nor is pParameters
  // outside the encoding of the depth clear value (82139328), which the own
  // float depth does not need; EndTiling passes stencil 0 like the original.
  if (flags & ~0x377u) {  // Not covered: the other bits are not read.
    UncoveredOperation(ctx, fmt::format("{}-uncovered-D3DRESOLVE.flags", source));
    flags &= 0x377u;
  }
  if (dest) {
    // An MSAA color source is averaged (all samples); 0 lets the original
    // pick all samples, as the own copy does. A select of one sample
    // (RB_COPY_CONTROL.copy_sample_select = value - 1, k0..k3) copies that
    // sample alone, as the Xenos and Xenia do: the copy of the scene the water
    // reads (flags 0x10, k0) keeps in its alpha the depth of the river bed or
    // 0, and an average gives the edges in front of the water a shallow depth
    // (white shore foam); the 640x360 4X pass of the user clip planes resolves
    // sample 0 too. A pair (k01, k23) is averaged with the other samples; a
    // select of samples a 2X source lacks stays a guard.
    const uint32_t select = (flags >> 4) & 7;
    const uint32_t source_samples = resources.Samples(
        src_index == 4 ? Get32(base, dev + 12448) : Get32(base, dev + 12432 + 4 * src_index));
    int32_t color_sample = -1;
    if (src_index < 4 && select && source_samples > 1 && select != (source_samples == 4 ? 7u : 5u)) {
      if (source_samples != 4 && select != 1 && select != 2)  // Averaged as the others.
        UncoveredOperation(ctx, fmt::format("{}-uncovered-RB_COPY_CONTROL.copy_sample_select", source));
      if (select <= 4 && select <= source_samples) color_sample = int32_t(select) - 1;
      if (Diagnostics() && resolve_select_records.fetch_add(1) < 8)
        host::Record("native-d3d-resolve-sample-select", fmt::format(
            "\"source_call\":\"{}\",\"flags\":{},\"select\":{},\"samples\":{},\"dest\":{},\"lr\":{},\"averaged\":{}",
            source, flags, select, source_samples, dest, ctx.lr, color_sample < 0));
    }
    // A depth source is resolved by the own full-screen pass: one sample
    // (select k0..k3 = 1..4) or, for all samples (0, or 5 and 7 when they
    // cover the source), their minimum. A pair of a 4X
    // source (k01, k23) is not covered.
    int32_t depth_sample = -1;
    if (src_index == 4) {
      if (select >= 1 && select <= 4) depth_sample = int32_t(select) - 1;
      else if (select == 6 || (select == 5 && source_samples > 2))  // The minimum of all samples.
        UncoveredOperation(ctx, fmt::format("{}-uncovered-RB_COPY_CONTROL.copy_sample_select_depth", source));
      if (depth_sample >= int32_t(std::max(source_samples, 1u))) {
        UncoveredOperation(ctx, fmt::format("{}-uncovered-RB_COPY_CONTROL.copy_sample_select_depth", source));
        depth_sample = -1;
      }
    }
    // An MSAA or depth source resolves the whole surface into a destination
    // of its size (the engine refuses a size change); a rectangle or a
    // destination point is not covered there.
    // Otherwise the copy is skipped (the destination keeps its content).
    bool copy = true;
    if ((src_index == 4 || source_samples > 1) && (rect || point)) {
      UncoveredOperation(ctx, fmt::format("{}-uncovered-resolve_region", source));
      copy = false;
    }
    // The destination is a 2D texture (fetch dword 5, dimension at bit 9).
    if (copy && ((Get32(base, dest + 0x30) >> 9) & 3) != 1) {
      UncoveredOperation(ctx, fmt::format("{}-uncovered-dest_fetch.dimension", source));
      copy = false;
    }
    if (copy) {
      profile::Timer profile(profile::kResolve);
      ResolveRequest request{};
      request.source_key = Backing(src_index == 4 ? Get32(base, dev + 12448) : Get32(base, dev + 12432 + 4 * src_index));
      if (!request.source_key) StopInOrder(ctx, fmt::format("{}-source-unbound", source), 88);
      request.dest_key = dest;
      const uint32_t dw1 = Get32(base, dest + 0x20), dw2 = Get32(base, dest + 0x24);
      request.dest_format = dw1 & 0x3F;
      request.dest_width = (dw2 & 0x1FFF) + 1;
      request.dest_height = ((dw2 >> 13) & 0x1FFF) + 1;
      if (rect) {
        request.has_rect = true;
        request.left = Get32(base, rect); request.top = Get32(base, rect + 4);
        request.right = Get32(base, rect + 8); request.bottom = Get32(base, rect + 12);
      }
      if (point) { request.dest_x = Get32(base, point); request.dest_y = Get32(base, point + 4); }
      request.sample = src_index == 4 ? depth_sample : color_sample;
      std::string evidence;
      uint64_t fence = 0;
      auto* dispatcher = host::ActiveDispatcher();
      // Posted, except the first 16 passes whose record keeps the engine evidence.
      const bool evidence_pass = Diagnostics() && pass_records.fetch_add(1) < 16;
      if (!dispatcher || dispatcher->PostedFailure(evidence)) {
        RecordPostedFailure(source);
        StopInOrder(ctx, fmt::format("{}-own-resolve-unproved", source), 88);
      }
      const bool done = evidence_pass ?
          dispatcher->Run([&request, &fence](Engine& engine, std::string& out) {
            return engine.ResolveSurface(request, fence, out); }, evidence) :
          dispatcher->Post([request](Engine& engine, std::string& out) {
            uint64_t posted_fence = 0;
            return engine.ResolveSurface(request, posted_fence, out); });
      if (!done) {
        RecordPostedFailure(source);
        StopInOrder(ctx, fmt::format("{}-own-resolve-unproved", source), 88);
      }
      // The header holds the guest virtual base (0xFF90C000 for the front buffer);
      // the device fetch constant a draw reads holds the physical one, so the
      // key is the physical base (with the virtual one no draw would find its
      // resolve destination).
      const uint32_t dest_physical = GuestPhysicalAlias(dw1 & 0xFFFFF000u);
      // Noted in order with the captures (on the stage when deferred): a
      // draw captured before this resolve must not bind the resolve destination.
      dispatcher->Defer([dest, dest_physical] { texture_content.NoteResolve(dest, dest_physical); });
      if (FrameTraceActive()) {
        TraceFrameResolveDest(dest);
        TraceFrameOp("resolve", fmt::format(
            "\"call\":\"{}\",\"flags\":{},\"source_index\":{},\"source\":{},\"dest\":{},\"dest_base\":{},"
            "\"dest_physical\":{},\"dest_format\":{},\"dest_size\":[{},{}],\"rect\":{},\"point\":{},\"sample\":{},\"lr\":{}",
            source, flags, src_index, request.source_key, dest, dw1 & 0xFFFFF000u, dest_physical, request.dest_format,
            request.dest_width, request.dest_height, request.has_rect, point != 0, depth_sample, ctx.lr));
      }
      if (fence || !evidence_pass) device_owner->NoteSubmission();
      // The CPU stores of 821398C8 that later CPU code reads, also for
      // the resolves of the original EndTiling, which calls it. The destination
      // Fence (+8) and ReadFence (+12) take +10908, so the Lock, the destruction
      // (8212D750) and title waits block on this resolve through the own
      // BlockOnFence. With +10908 = 0 the original queues the destination on the
      // deferred list +13508 instead; the own counter starts at 3. Bit 0x08 of
      // +10942 gates three flush events that only the original Resolve emits.
      const uint32_t fence10908 = Get32(base, dev + 10908);
      // Not covered: without the census the destination takes fence 0 (no wait), not the deferred list.
      if (!fence10908) UncoveredOperation(ctx, fmt::format("{}-deferred-fence-list-uncovered", source));
      Put32(base, dest + 8, fence10908);
      Put32(base, dest + 12, fence10908);
      Put8(base, dev + 10942, Get8(base, dev + 10942) & 0xF7);
      if (evidence_pass)
        host::Record("native-d3d-resolve", fmt::format(
            "\"source_call\":\"{}\",\"flags\":{},\"source\":{},\"dest\":{},\"dest_format\":{},\"lr\":{},"
            "\"engine\":{}", source, flags, request.source_key, dest, request.dest_format, ctx.lr, evidence));
    }
  }
  if (flags & 0x300) {
    const uint32_t clear_flags = ((flags & 0x100) && src_index < 4 ? 1u << src_index : 0u) |
                                 ((flags & 0x200) ? 0x30u : 0u);
    OwnClear(ctx, base, dev, clear_flags, {0, 0, 0, 0}, GuestColor(base, color), z, stencil, source);
  }
}
}  // namespace gpu

extern "C" void __imp__sub_821398C8(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_821398C8) {
  if (!gpu::Active()) { __imp__sub_821398C8(ctx, base); return; }
  HookScope scope("Resolve");
  if (!device_owner || !device_owner->live() || ctx.r3.u32 != device_owner->device()) StopInOrder(ctx, "Resolve-device", 88);
  TakePendingDraw(&ctx, base, "Resolve");
  // The register shadows the original writes (10776..10820) feed only its own
  // serialized commit, which the D3D boundary does not run.
  // ClearStencil is the stack argument at the caller's r1 + 92 (EndTiling stores it there).
  OwnResolve(ctx, base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r10.u32,
             float(ctx.f1.f64), Get32(base, ctx.r1.u32 + 92), "Resolve");
}

// D3DDevice_EndTiling(device, ResolveFlags, pResolveRects, pDestTexture, pClearColor,
// Z, -, Stencil, pParameters). Without EDRAM tiles the native pass resolves the
// whole surface once and applies the final clear of the flags; the per-tile
// clears of the original only reset EDRAM between tiles. CPU stores that the
// setters read are kept; the end-of-tiling command buffers are not reproduced.
extern "C" void __imp__sub_8213AC80(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_8213AC80) {
  if (!gpu::Active()) { __imp__sub_8213AC80(ctx, base); return; }
  HookScope scope("EndTiling");
  if (!device_owner || !device_owner->live() || ctx.r3.u32 != device_owner->device()) StopInOrder(ctx, "EndTiling-device", 88);
  TakePendingDraw(&ctx, base, "EndTiling");
  const uint32_t dev = ctx.r3.u32;
  // Not covered: the tile rectangles are not read (the whole surface resolves).
  if (ctx.r5.u32) UncoveredOperation(ctx, "EndTiling-resolve-rects-uncovered");
  // The original skips its whole resolve loop, clears included, without a
  // destination. The final clear targets the bound surfaces after the
  // backings end, as the console leaves the EDRAM of those surfaces cleared.
  const uint32_t resolve_flags = ctx.r4.u32;
  if (ctx.r6.u32)
    OwnResolve(ctx, base, dev, resolve_flags & ~0x300u, 0, ctx.r6.u32, 0, 0, 0.0f, 0, "EndTiling");
  EndBackings();
  if (ctx.r6.u32 && (resolve_flags & 0x300))
    OwnResolve(ctx, base, dev, resolve_flags, 0, 0, 0, ctx.r7.u32, float(ctx.f1.f64), 0, "EndTiling");
  Put32(base, dev + 12716, 0);
  Put32(base, dev + 12708, 0);
  const uint8_t b = Get8(base, dev + 10940) & 0xDF;
  Put8(base, dev + 10940, b);
  Put8(base, dev + 10943, Get8(base, dev + 10943) & 0xCF);
  uint32_t bit = 0;
  if (!(b & 0x08) && !(b & 0x04) && !Get8(base, dev + 12179)) {
    const uint8_t now = Get8(base, dev + 10940);
    if (now & 0x10) bit = 1;
    else if (now & 0x20) {
      bit = 1;
      for (unsigned i = 0; i < 5 && bit; ++i) {
        const uint32_t slot = Get32(base, dev + 12432 + 4 * i), tiled = Get32(base, dev + 12720 + 4 * i);
        if (tiled != slot && slot) bit = 0;
      }
    }
  }
  Put32(base, dev + 12704, 0);
  Put32(base, dev + 10932, 0);
  Put32(base, dev + 10936, 0);
  Put32(base, dev + 12700, 0xFFFFFFFF);
  Put8(base, dev + 10940, uint8_t((Get8(base, dev + 10940) & 0xFE) | bit));
  const uint32_t frame = (ctx.r1.u32 - 512) & ~15u;
  CallGuest(ctx, base, __imp__sub_8212AC00, frame, dev, 0x8208B398);
  ctx.r3.u64 = 0;  // S_OK; the own pass has no tiling allocation to fail.
  if (FrameTraceActive())
    TraceFrameOp("end-tiling", fmt::format("\"flags\":{},\"dest\":{},\"lr\":{}", ctx.r4.u32, ctx.r6.u32, ctx.lr));
  if (Diagnostics() && pass_records.fetch_add(1) < 16)
    gpu::host::Record("native-d3d-end-tiling", fmt::format(
        "\"flags\":{},\"dest\":{},\"color_ptr\":{},\"lr\":{},\"tiling_infrastructure\":false",
        ctx.r4.u32, ctx.r6.u32, ctx.r7.u32, ctx.lr));
}
