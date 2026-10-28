// Draw layer of the D3D boundary: D3DDevice_BeginVertices, the own
// DrawIndexedVertices and DrawVertices, the checkpoint at the first producer,
// and the scene flush; the translation it uses has units of its own (shaders,
// vertex layout, textures, buffers, state) and so do the diagnostics.
//
// 821375A0 is D3DDevice_BeginVertices, specialized by LTCG to
// (D3DPT_TRIANGLEFAN = 5, VertexCount = 4, Stride = 20). The original commits
// the dirty state (masks +0..+32, with CPU effects in the shader commit
// 82141178), reserves the vertex data with 8212EA18, writes the draw packets
// and returns the vertex pointer. The title writes the vertices and ends with
// the inline EndVertices: device+48 = device+13428.
//
// The state commit runs original, so its CPU effects stay the title's; its
// packets land in an own discard buffer that no consumer reads (cursor +48,
// segment end +52 and soft limit +56 point there only during the call). The vertex data goes to an own
// area. The draw is kept as pending and taken by the next BeginVertices, the
// producer checkpoint or the stop. The quad is drawn on the GPU. Its
// request is built right after the commit by OwnDraw (inline mode: the state
// the commit left, the inline fetch constant 95 of the packet on the own vertex
// area); the take, before the next own operation of the thread (draw, clear,
// resolve, tiling, Swap), reads the vertices the title wrote and posts the
// draw. After the call +48, +56 and +13428 hold the guard cursor again, so the
// inline EndVertices keeps every other inline writer on the guarded F608.
#include "gpu/d3d/draw.h"
#include "diagnostics/incident.h"
#include "gpu/d3d/draw_capture.h"
#include "gpu/d3d/d3d.h"
#include "gpu/diagnostics/diagnostics.h"
#include "gpu/d3d/hooks.h"
#include "gpu/d3d/device.h"
#include "gpu/d3d/resources.h"
#include "gpu/d3d/pass.h"
#include "gpu/d3d/state.h"
#include "gpu/d3d/frame.h"
#include "gpu/diagnostics/measure.h"
#include "gpu/shaders/shaders.h"
#include "gpu/shaders/pipelines.h"
#include "gpu/buffers/buffers.h"
#include "gpu/buffers/vertex_layout.h"
#include "gpu/textures/textures.h"
#include "gpu/diagnostics/frame_state.h"
#include "gpu/diagnostics/census.h"
#include "gpu/diagnostics/frame_trace.h"
#include "gpu/diagnostics/start_evidence.h"
#include "gpu/common/guest_memory.h"
#include "gpu/common/hash.h"
#include "gpu/common/records.h"
#include "gpu/backend/dispatcher.h"
#include "gpu/common/sha256.h"
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
#include <optional>
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
#include "gpu/diagnostics/sampler.h"

using namespace gpu;

namespace gpu {
InlineDraw inline_draw;
std::atomic<uint64_t> inline_quads_drawn{0};
// Posts the built quad with the vertices the title wrote (defined after
// OwnDraw), outside inline_draw.mutex (a stop releases the inline areas); ctx
// null: the stop, where the quad is dropped.
std::string PostInlineQuad(PPCContext* ctx, uint8_t* base, const PendingDraw& d, std::shared_ptr<InlineBuild> build,
                           const char* by);
// The 821083B8 hook and the reserves of its tail admitted.
std::atomic<uint32_t> scene_item_thread{0};  // Guest thread inside 821083B8.
std::atomic<uint64_t> scene_items{0}, scene_flushes{0};

// Reads the vertices of the pending draw once the title has written them.
// Not executed on the GPU yet: the record is the input of the draw layer.
void TakePendingDraw(PPCContext* ctx, uint8_t* base, const char* by) {
  if (!inline_draw.has_pending.load(std::memory_order_acquire)) return;
  PendingDraw d{};
  std::shared_ptr<InlineBuild> build;
  uint64_t taken = 0;
  {
    std::lock_guard lock(inline_draw.mutex);
    if (!inline_draw.has_pending.load(std::memory_order_relaxed)) return;
    d = inline_draw.pending;
    inline_draw.has_pending.store(false, std::memory_order_relaxed);
    build = std::move(inline_draw.build);
    taken = ++inline_draw.taken;
  }
  const std::string gpu = PostInlineQuad(ctx, base, d, std::move(build), by);
  if (Diagnostics() && taken <= 16) {
    std::string floats;
    for (uint32_t i = 0; i < d.count * d.stride / 4; ++i) {
      const float f = GetFloat(base, d.vertices + 4 * i);
      floats += std::isfinite(f) ? fmt::format("{}{}", i ? "," : "", f) : std::string(i ? ",null" : "null");
    }
    host::Record("native-d3d-inline-draw", fmt::format(
        "\"call\":{},\"taken_by\":\"{}\",\"primitive\":{},\"count\":{},\"stride\":{},\"vertices\":{},"
        "\"vertex_words\":[{}],\"shader12684\":{},\"shader12688\":{},\"target\":{},\"depth\":{},\"lr\":{},"
        "\"guest_thread\":{},{}", d.call, by, d.primitive, d.count, d.stride, d.vertices, floats, d.shader12684,
        d.shader12688, d.target, d.depth, d.lr, d.thread, gpu));
  }
}

bool EnsureInlineAreas() {
  std::lock_guard lock(inline_draw.mutex);
  if (inline_draw.discard) return true;
  auto* ts = rex::runtime::ThreadState::Get();
  if (!ts) return false;
  inline_draw.memory = ts->memory();
  inline_draw.discard = inline_draw.memory->SystemHeapAlloc(kDiscardBytes, 0x1000);
  inline_draw.vertices = inline_draw.memory->SystemHeapAlloc(kVertexBytes, 0x1000);
  inline_draw.patch = inline_draw.memory->SystemHeapAlloc(kPatchBytes, 0x1000);
  host::Record("native-d3d-inline-areas", fmt::format(
      "\"discard\":{},\"discard_bytes\":{},\"discard_limit\":{},\"vertices\":{},\"vertex_bytes\":{},\"patch\":{},"
      "\"patch_bytes\":{},\"heap\":\"system\",\"consumer\":false", inline_draw.discard, kDiscardBytes, kDiscardLimit,
      inline_draw.vertices, kVertexBytes, inline_draw.patch, kPatchBytes));
  return inline_draw.discard && inline_draw.vertices && inline_draw.patch;
}
}  // namespace gpu

namespace gpu {
uint64_t InlineDrawCalls() { return inline_draw.calls; }
// Called by StopInOrder before the engine is released.
void ReleaseInlineDraw() {
  auto* ts = rex::runtime::ThreadState::Get();
  if (ts) TakePendingDraw(nullptr, ts->memory()->virtual_membase(), "stop");
  std::lock_guard lock(inline_draw.mutex);
  if (!inline_draw.memory) return;
  if (inline_draw.discard) inline_draw.memory->SystemHeapFree(inline_draw.discard);
  if (inline_draw.vertices) inline_draw.memory->SystemHeapFree(inline_draw.vertices);
  if (inline_draw.patch) inline_draw.memory->SystemHeapFree(inline_draw.patch);
  host::Record("native-d3d-inline-release", fmt::format(
      "\"calls\":{},\"taken\":{},\"quads_drawn\":{},\"discard\":{},\"vertices\":{},\"patch\":{},\"scene_items\":{},"
      "\"scene_flushes\":{}", inline_draw.calls, inline_draw.taken, inline_quads_drawn.load(), inline_draw.discard,
      inline_draw.vertices, inline_draw.patch, scene_items.load(), scene_flushes.load()));
  inline_draw.discard = inline_draw.vertices = inline_draw.patch = 0;
  inline_draw.memory = nullptr;
}
}  // namespace gpu

// Builds the quad request of the BeginVertices that just committed
// (defined after OwnDraw); stops in order when the draw is uncovered.
void BuildInlineQuad(const PPCContext& entry, uint8_t* base, uint32_t vertices, PPCContext& ctx);

// Vertex reservation of the ring. Only the BeginVertices call inside the own
// scope is admitted: it gets the own vertex area. The ring-only accounting of
// the original (+14916 against +14920, kick threshold in +52/+56) is not
// reproduced. Any other arrival is an uncovered ring allocation.
extern "C" void __imp__sub_8212EA18(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_8212EA18) {
  if (!gpu::Active()) { __imp__sub_8212EA18(ctx, base); return; }
  HookScope scope("EA18-vertex-reserve");
  if (inline_draw.scope_thread.load() != GuestThread() || ctx.lr != 0x8213783C)
    StopInOrder(ctx, "EA18-ring-allocation-guard", 86);
  const uint32_t bytes = ctx.r4.u32 * 4, align = ctx.r5.u32;
  if (!bytes || bytes > kVertexBytes || !align || (align & (align - 1)) || align > 0x1000)
    StopInOrder(ctx, "EA18-request-uncovered", 88);
  std::lock_guard lock(inline_draw.mutex);
  uint32_t at = (inline_draw.vertex_cursor + align - 1) & ~(align - 1);
  if (at + bytes > kVertexBytes) at = 0;  // The previous draw was taken before this call.
  inline_draw.vertex_cursor = at + bytes;
  inline_draw.scope_vertices = inline_draw.vertices + at;
  ctx.r3.u64 = inline_draw.scope_vertices;
}

// D3DDevice_BeginVertices, LTCG-specialized; see the header of this file.
extern "C" void __imp__sub_821375A0(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_821375A0) {
  StateNoteDraw(ctx, base, 2);  // Both modes.
  if (!gpu::Active()) { __imp__sub_821375A0(ctx, base); return; }
  HookScope scope("BeginVertices");
  if (!device_owner || !device_owner->live() || ctx.r3.u32 != device_owner->device())
    StopInOrder(ctx, "BeginVertices-device", 88);
  if (ctx.r4.u32 != 5 || ctx.r5.u32 != 4 || ctx.r6.u32 != 20) StopInOrder(ctx, "BeginVertices-signature", 88);
  const uint32_t dev = ctx.r3.u32, lr = ctx.lr, thread = GuestThread();
  constexpr uint32_t kCursor = DeviceOwner::kCursor;
  // 10940 bit 0 selects the predicated-tiling packet path (13232 buffers).
  if (Get8(base, dev + 10940) & 1) StopInOrder(ctx, "BeginVertices-predicated-path-uncovered", 86);
  if (Get8(base, dev + 10941) & 0x20) StopInOrder(ctx, "BeginVertices-allocation-failed-flag", 88);
  if (Get32(base, dev + 48) != kCursor || Get32(base, dev + 56) != 0 || inline_draw.scope_thread.load())
    StopInOrder(ctx, "BeginVertices-cursor-state", 88);
  TakePendingDraw(&ctx, base, "BeginVertices");
  if (!EnsureInlineAreas()) StopInOrder(ctx, "BeginVertices-own-areas", 88);
  const PPCContext entry = ctx;  // Device and arguments for the quad request.
  std::array<uint32_t, 10> masks{};
  for (unsigned i = 0; i < 10; ++i) masks[i] = Get32(base, dev + 4 * i);
  // Constant writers (8213F9F0) bound their copy by the segment end +52.
  const uint32_t end52 = Get32(base, dev + 52), scope_end = inline_draw.discard + kDiscardBytes - 256;
  Put32(base, dev + 48, inline_draw.discard);
  Put32(base, dev + 52, scope_end);
  Put32(base, dev + 56, inline_draw.discard + kDiscardLimit);
  inline_draw.scope_vertices = 0;
  inline_draw.scope_thread.store(thread);
  __imp__sub_821375A0(ctx, base);
  inline_draw.scope_thread.store(0);
  const uint32_t vertices = ctx.r3.u32, end = Get32(base, dev + 13428), cursor = Get32(base, dev + 48);
  const uint32_t discarded = end - inline_draw.discard, scope_end_after = Get32(base, dev + 52);
  Put32(base, dev + 48, kCursor);
  Put32(base, dev + 52, end52);
  Put32(base, dev + 56, 0);
  if (!vertices || vertices != inline_draw.scope_vertices || Get32(base, dev + 13432) != vertices ||
      Get32(base, dev + 13440) != 20 || end < inline_draw.discard || discarded > kDiscardLimit + 256 ||
      cursor < inline_draw.discard || cursor > end || scope_end_after != scope_end)
    StopInOrder(ctx, "BeginVertices-return-unproved", 88);
  Put32(base, dev + 13428, kCursor);  // Inline EndVertices stores it in +48.
  std::array<uint32_t, 10> after{};
  for (unsigned i = 0; i < 10; ++i) after[i] = Get32(base, dev + 4 * i);
  uint64_t call = 0;
  {
    std::lock_guard lock(inline_draw.mutex);
    call = ++inline_draw.calls;
    inline_draw.pending = {vertices, 5, 4, 20, Get32(base, dev + 12684), Get32(base, dev + 12688),
                           Get32(base, dev + 12432), Get32(base, dev + 12448), lr, thread, call};
    inline_draw.has_pending.store(true, std::memory_order_release);
  }
  if (Diagnostics() && call <= 16)
    gpu::host::Record("native-d3d-begin-vertices", fmt::format(
        "\"call\":{},\"lr\":{},\"guest_thread\":{},\"vertices\":{},\"discarded_bytes\":{},\"commit_bytes\":{},"
        "\"masks_before\":[{}],\"masks_after\":[{}],\"cursor_restored\":{},\"packets_consumed\":false",
        call, lr, thread, vertices, discarded, cursor - inline_draw.discard, fmt::join(masks, ","),
        fmt::join(after, ","), kCursor));
  // The quad request from the state of this commit (inline fetch 95).
  BuildInlineQuad(entry, base, vertices, ctx);
}

// BEFORE 821024A8/LR 82108330: arrival at the first producer. The arrival is
// recorded once and the original runs; its draws reach the
// DrawIndexedVertices capture below. Other callers run the original. The
// probe build owns the same hook.
#if !defined(NARUTORISE_NATIVE_RENDERER_PROBE)
namespace gpu {
std::atomic<uint64_t> producer_arrivals{0};
std::atomic<uint64_t> producer_calls{0};  // Every caller (82105648 for the scene items too).
thread_local ProducerBefore producer_before;
thread_local uint64_t thread_producer_calls = 0;
}  // namespace gpu
extern "C" void __imp__sub_821024A8(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_821024A8) {
  uint64_t call = 0;
  if (gpu::Active()) {
    call = ++producer_calls;
    ++thread_producer_calls;
  }
  if (gpu::Active() && ctx.lr == 0x82108330) {
    if (!device_owner || !device_owner->live()) StopInOrder(ctx, "producer-without-live-device", 88);
    const uint64_t arrival = producer_arrivals.fetch_add(1) + 1;
    producer_before = {call, arrival, thread_producer_calls, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r1.u32, false};
    if (arrival == 1) {
      TakePendingDraw(&ctx, base, "producer");
      if (Diagnostics()) gpu::host::Record("native-d3d-producer-checkpoint", fmt::format(
          "\"lr\":{},\"guest_thread\":{},\"r3\":{},\"r4\":{},\"r5\":{},\"r1\":{},\"inline_draws\":{},"
          "\"continues\":true", ctx.lr, GuestThread(), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r1.u32,
          inline_draw.calls));
    }
  }
  __imp__sub_821024A8(ctx, base);
}
#endif

namespace gpu {
std::atomic<uint64_t> own_draws{0};
std::atomic<uint64_t> start_draws{0};  // Own draws with the shaders of the START lot.
std::atomic<uint64_t> start_first_swap{0};  // Own Swaps before the first draw with the START shaders.
std::atomic<bool> draw_capture_recorded{false};

// Runs an original whose packets must reach no consumer, as BeginVertices
// does with its commit: cursor +48, segment end +52 and soft limit +56 point
// into the own discard buffer during the call and get the guard values back.
// Its CPU stores stay the title's. Returns the bytes written to the buffer.
std::atomic<uint32_t> discard_thread{0};  // Guest thread inside RunOverDiscard.
std::atomic<uint64_t> discard_leaves{0};
std::atomic<uint64_t> unpredicated_commits{0};
// Between BeginTiling and EndTiling the original keeps 10940 bit 0
// set, and DrawIndexedVertices then wraps its packet in a predication and
// records 16 bytes in the secondary buffer +13232 (cursor +13240), which only
// the tile predication of the original EndTiling consumes. The own pass has no
// tiles: with clear_predication the call runs with bit 0 clear (its commit
// helpers test only 0x10, 0x40 and 0x80), its packet takes the plain path into
// the discard buffer, and the bit is set again afterwards.
uint32_t RunOverDiscard(PPCContext& ctx, uint8_t* base, uint32_t dev, GuestFunction fn, std::string_view name,
                        bool clear_predication) {
  constexpr uint32_t kCursor = DeviceOwner::kCursor;
  // Nested call (821384F0 inside 82138848 under the own Swap): the cursor
  // already points into the discard buffer.
  if (discard_thread.load() && discard_thread.load() == GuestThread()) {
    fn(ctx, base);
    return 0;
  }
  const bool predicated = Get8(base, dev + 10940) & 1;
  if (predicated && !clear_predication) StopInOrder(ctx, fmt::format("{}-predicated-path-uncovered", name), 86);
  if (Get32(base, dev + 48) != kCursor || Get32(base, dev + 56) != 0 || inline_draw.scope_thread.load())
    StopInOrder(ctx, fmt::format("{}-cursor-state", name), 88);
  if (!EnsureInlineAreas()) StopInOrder(ctx, fmt::format("{}-own-areas", name), 88);
  const uint32_t end52 = Get32(base, dev + 52), scope_end = inline_draw.discard + kDiscardBytes - 256;
  const uint32_t inline_end = Get32(base, dev + 13428);  // Only BeginVertices writes it, in its own scope.
  Put32(base, dev + 48, inline_draw.discard);
  Put32(base, dev + 52, scope_end);
  Put32(base, dev + 56, inline_draw.discard + kDiscardLimit);
  discard_thread.store(GuestThread());
  if (predicated) Put8(base, dev + 10940, Get8(base, dev + 10940) & 0xFE);
  fn(ctx, base);
  if (predicated) {
    Put8(base, dev + 10940, Get8(base, dev + 10940) | 1);
    ++unpredicated_commits;
  }
  discard_thread.store(0);
  const uint32_t cursor = Get32(base, dev + 48);
  // No original of these scopes moves the segment end or the inline end.
  const bool bounds_kept = Get32(base, dev + 52) == scope_end && Get32(base, dev + 13428) == inline_end;
  Put32(base, dev + 48, kCursor);
  Put32(base, dev + 52, end52);
  Put32(base, dev + 56, 0);
  if (cursor < inline_draw.discard || cursor > inline_draw.discard + kDiscardLimit + 256 || !bounds_kept)
    StopInOrder(ctx, fmt::format("{}-discard-unproved", name), 88);
  return cursor - inline_draw.discard;
}
}  // namespace gpu

// D3DDevice_DrawIndexedVertices(device, primitive, base vertex, start index,
// index count) and D3DDevice_DrawVertices(device, primitive, start vertex,
// vertex count) share the commit helpers and the packet paths (plain and
// predicated by 10940 bit 0); the own draw of DrawVertices uses the indices
// start .. start + count - 1.
extern "C" void __imp__sub_82137E00(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82137A18(PPCContext& __restrict, uint8_t*);
namespace gpu {
// REX_NATIVE_PROBE_TEXTURE=<texture key>, with the diagnostics: each draw
// that samples the texture records the positions of the first four vertices
// of its stream from the base vertex and its vertex constants c0..c3
// (native-d3d-probe-draw), at most kProbeDraws, without reading the frame
// back; the capture keeps the first four rows of the vertex shadow.
constexpr uint64_t kProbeDraws = 40000;
uint32_t ProbeTexture() {
  static const uint32_t value = [] {
    const char* text = std::getenv("REX_NATIVE_PROBE_TEXTURE");
    return Diagnostics() && text && *text ? uint32_t(std::strtoul(text, nullptr, 10)) : 0u;
  }();
  return value;
}
std::atomic<uint64_t> probe_draws{0};
// The pipeline take of the batched draws (DrawRequest::PipelineTake): the
// prefetch take by default (the draw never skips); REX_NATIVE_PSO_ASYNC=0
// turns the asynchronous threads off (the draw waits for or creates its
// pipeline, nothing posted before the batch); REX_NATIVE_PSO_ASYNC=1 brings
// back the skip take, with the diagnostics only (the A/B of before).
DrawRequest::PipelineTake BatchedPipelineTake() {
  static const DrawRequest::PipelineTake value = [] {
    const char* text = std::getenv("REX_NATIVE_PSO_ASYNC");
    const std::string_view option = text ? text : "";
    if (option == "0") return DrawRequest::PipelineTake::kSync;
    if (option == "1" && Diagnostics()) return DrawRequest::PipelineTake::kSkip;
    return DrawRequest::PipelineTake::kPrefetch;
  }();
  return value;
}

// The quad of D3DDevice_BeginVertices (TRIANGLEFAN, 4 vertices of 20
// bytes). The commit wrote the inline fetch constant only into its packet
// (SET_CONSTANT 0x48BA..0x48BF): fetch 95 = the physical alias of the vertex
// area | 3, 20 words, endian 2 (8in32). OwnDraw builds the request from the
// state the commit left without running a draw entry, and keeps it here.
struct InlineBuild {
  uint32_t vertices = 0;              // Own vertex area (guest virtual).
  static constexpr uint32_t kFetch = 95, kBytes = 80, kEndian = 2;
  DrawRequest request;
  uint32_t slot = UINT32_MAX;         // Stream slot of fetch 95, filled at the take.
  std::string record;
  bool census_skipped = false;        // A census guard skipped the own quad.
};
// The first register field of a draw that the own request does not
// translate and whose value leaves the covered one, or null. Covered values
// are those of every START draw; fields the console and the own draw treat
// alike (rounding, quad order, Hi-Z, NaN retention) are not checked. The
// registers, the bound headers and their EDRAM bases come from the capture.
template <class Reg>
const char* UncoveredDrawField(const DrawCapture& c, const Reg& reg, const DrawRequest& request, uint32_t mode,
                               uint32_t vte, uint32_t color_control) {
  auto regf = [&](uint32_t n) { const uint32_t v = reg(n); float f; std::memcpy(&f, &v, 4); return f; };
  // Render target 0 left without an own surface by the D330 (format outside).
  const uint32_t color_header = c.targets[0];
  ResourceRegistry::Entry target{};
  if (!request.depth_only &&  // No color target in a depth-only draw.
      resources.Find(color_header, target) && target.kind == ResourceRegistry::Kind::Surface && !target.host_surface)
    if (const char* field = UncoveredColorField(c.registers[kColorFormatRegister], c.registers[kColorInfoRegister]))
      return field;
  // Two bound headers on the same EDRAM base.
  if (SharedEdramBase(c.targets, c.target_edram)) return "EDRAM.base_shared";
  // PA_SU_SC_MODE_CNTL (0x2205): cull and face are translated.
  if ((mode & 3) == 3) return "PA_SU_SC_MODE_CNTL.cull_front_and_back";
  if ((mode >> 3) & 3) return "PA_SU_SC_MODE_CNTL.poly_mode";
  // Poly_offset (bits 11..13) is translated into the depth bias of the
  // request; the own draws are triangles, so the para offset never applies.
  // A depth-only draw binds only its depth target, which gives the samples.
  const uint32_t samples_header = request.depth_only ? c.targets[4] : color_header;
  if (!((mode >> 15) & 1) && resources.Samples(samples_header) > 1) return "PA_SU_SC_MODE_CNTL.msaa_enable";
  if ((mode >> 19) & 1) return "PA_SU_SC_MODE_CNTL.provoking_vtx_last";
  if ((mode >> 20) & 1) return "PA_SU_SC_MODE_CNTL.persp_corr_dis";
  // The PSO has no strip cut value: a reset index only matters for strips.
  if (((mode >> 21) & 1) && request.topology == Topology::kTriangleStrip) return "PA_SU_SC_MODE_CNTL.multi_prim_ib_ena";
  // Window offset (0x2080, index 16 of the 0x2000 block): not applied.
  if (reg(0x2010) & 0x7FFF7FFF) return "PA_SC_WINDOW_OFFSET";
  // PA_CL_VTE_CNTL (0x2206): the six enables are guarded with the transform;
  // XY and Z divided by W, W0 given as 1/W (vtx_w0_fmt), as in D3D9.
  if (((vte >> 8) & 7) != 4) return "PA_CL_VTE_CNTL.vtx_format";
  // PA_CL_CLIP_CNTL (0x2204): D3D clip space. The user clip planes
  // (ucp_ena) are SV_ClipDistance of every VS of the fork, from the planes at
  // shared +384 (DrawClipPlanes); clipping only, by the vertex shader: the cull
  // only mode and the pixel shader modes (ps_ucp_mode) are not translated.
  const uint32_t clip = reg(0x2204);
  if ((clip & 0x3F) && ((clip >> 14) & 3)) return "PA_CL_CLIP_CNTL.ps_ucp_mode";
  if ((clip & 0x3F) && ((clip >> 17) & 1)) return "PA_CL_CLIP_CNTL.ucp_cull_only_ena";
  if ((clip >> 16) & 1) return "PA_CL_CLIP_CNTL.clip_disable";
  if (!((clip >> 19) & 1)) return "PA_CL_CLIP_CNTL.dx_clip_space_def";
  // PA_SU_VTX_CNTL (0x2302): g_HalfPixelOffset assumes the D3D9 pixel center.
  if (reg(0x2302) & 1) return "PA_SU_VTX_CNTL.pix_center";
  // RB_MODECONTROL (0x2208): color and depth; depth only (5) with no PS.
  if ((reg(0x2208) & 7) != (request.depth_only ? 5u : 4u)) return "RB_MODECONTROL.edram_mode";
  // RB_COLORCONTROL (0x2202): alpha test and alpha to mask are translated.
  // The alpha to mask dither offsets (bits 24..31) are dropped: plain alpha
  // to coverage.
  if (color_control & 0x00FFFFE0) return "RB_COLORCONTROL.reserved";
  // Viewport with sign: the request takes the absolute scales, which is the
  // console mapping only for X right and Y down; the depth range must fit [0, 1].
  if (!(regf(0x210F) > 0)) return "PA_CL_VPORT_XSCALE.sign";
  if (!(regf(0x2111) < 0)) return "PA_CL_VPORT_YSCALE.sign";
  const float z0 = regf(0x2114), z1 = z0 + regf(0x2113);
  if (!(std::min(z0, z1) >= 0 && std::max(z0, z1) <= 1)) return "PA_CL_VPORT_Z.range";
  // A depth-only draw writes no color: render targets and blend do not apply.
  if (request.depth_only) return nullptr;
  // MRT: only render target 0 is drawn.
  for (unsigned i = 1; i < 4; ++i)
    if (c.targets[i]) return "RB_COLOR1_3_INFO.render_target";
  // RB_BLENDCONTROL0: CONSTANT_ALPHA (14, 15) in a color factor uses the blend
  // color RGBA, which is the console value only when R = G = B = A.
  const uint32_t bc = request.blend_control;
  const auto& k = request.blend_factor;
  for (uint32_t f : {bc & 0x1F, (bc >> 8) & 0x1F})
    if ((f == 14 || f == 15) && !(k[0] == k[3] && k[1] == k[3] && k[2] == k[3])) return "RB_BLENDCONTROL0.constant_alpha";
  for (uint32_t f : {bc & 0x1F, (bc >> 8) & 0x1F, (bc >> 16) & 0x1F, (bc >> 24) & 0x1F})
    if (f == 2 || f == 3 || f > 16) return "RB_BLENDCONTROL0.factor";
  for (uint32_t op : {(bc >> 5) & 7, (bc >> 21) & 7})
    if (op > 4) return "RB_BLENDCONTROL0.comb_fcn";
  return nullptr;
}

// Own D3DDevice_DrawIndexedVertices. The original runs over the own
// discard buffer, as BeginVertices does: its commit keeps the CPU effects
// (dirty masks, the vertex fetch patch of the bound VS by the declaration) and
// its packets reach no consumer. The draw is then built from the device:
//   - shaders: XenosRecomp DXIL from the disk cache (REX_NATIVE_SHADER_INDEX),
//     identified by the virtual part of the container, with the PS microcode
//     equal to the SDB and the VS equal outside its vertex fetches;
//   - input layout: vertex elements of the VS container and the patched fetch;
//   - vertex streams: fetch constants at +0x480; indices: header at +12428;
//   - constants: VS at +0x780 and PS at +0x1780 (256 float4 each);
//   - state: Xenos register blocks committed by the original (base register
//     0x2200 at +10548, 0x2100 at +10444, 0x2000 at +10368);
//   - pixel shader textures: fetch constant at +0x480 + 24 * sampler register
//     (see BindTexture).
//   - alpha test: the PS variant with SPEC_CONSTANT_ALPHA_TEST (see LoadShader).
//   - stencil: RB_DEPTHCONTROL and RB_STENCILREFMASK in the PSO.
// Draws with uncovered inputs (vertex textures, texture layouts outside
// DecodeTexture, alpha functions other than GREATER, GEQUAL and ALWAYS, a
// back-face stencil ref or mask of its own, other formats or primitives) stop
// at a guard with the reason recorded.
//
// The own draw is split in two. CaptureDraw
// runs on the title's render thread and copies what only it can read or run:
// the original commit over the discard buffer, the constants and the fetch
// constants that changed (deltas), the register blocks after the commit, the
// bound headers and the index buffer words, the shader entries with their
// identification and the vertex layout (82140540 runs as guest code).
// TranslateDraw reads the capture alone, no device word and no guest context,
// and posts the request; its guards go through a GuardSink, which inline stops
// in order with the context saved before the commit. Inline both run on the
// title thread, one after the other; deferred, TranslateDraw runs on the
// translation stage. The evidence records that need the context stay on the
// title after the translation (OwnDrawBody).
// A diagnostic record built on its first use (a guard, an evidence
// draw); never on a production draw that emits none.
template <class F>
class LazyRecord {
 public:
  explicit LazyRecord(F f) : f_(std::move(f)) {}
  const std::string& get() const {
    if (!built_) { value_ = f_(); built_ = true; }
    return value_;
  }
  operator const std::string&() const { return get(); }
 private:
  F f_;
  mutable std::string value_;
  mutable bool built_ = false;
};
template <class F> LazyRecord<F> MakeLazyRecord(F f) { return LazyRecord<F>(std::move(f)); }
template <class F> std::string operator+(const LazyRecord<F>& a, const char* b) { return a.get() + b; }
template <class F> std::string operator+(const LazyRecord<F>& a, const std::string& b) { return a.get() + b; }
template <class F> std::string operator+(const std::string& a, const LazyRecord<F>& b) { return a + b.get(); }
// The detail of a guard, built when the guard records it.
const std::string& DetailText(const std::string& detail) { return detail; }
template <class F> const std::string& DetailText(const LazyRecord<F>& detail) { return detail.get(); }
// The guard of a draw. In the guard census and in the product the guard
// records its first occurrences and skips the own draw (CensusSkip, caught by
// OwnDraw); otherwise it stops through the destination. The detail (a string
// or a LazyRecord) is built only when recorded.
struct DrawGuard {
  const DrawCapture& c;
  GuardSink& sink;
  template <class Detail>
  [[noreturn]] void operator()(std::string_view reason, const Detail& detail) const {
    const bool census_skip = SkipUncoveredDraws();  // The census, or the product.
    const uint64_t occurrence = census_skip ? NoteCensus(std::string(reason)) : 0;
    if (!census_skip || (occurrence && occurrence <= kCensusRecords))
      host::Record("native-d3d-draw-guard", fmt::format(
          "\"entry\":\"{}\",\"reason\":\"{}\",\"own_draws\":{},\"producer_calls\":{},\"unpredicated_commits\":{},"
          "\"texture_uploads\":{},\"texture_binding_reuses\":{},\"lr\":{},\"guest_thread\":{},\"primitive\":{},\"index_count\":{},"
          "\"draw\":{},{}{}",
          c.name, reason, own_draws.load(), producer_calls.load(), unpredicated_commits.load(), texture_uploads.load(),
          texture_binding_reuses.load(), c.lr, c.thread, c.primitive, c.index_count, c.n ? c.n : own_draws.load() + 1,
          DetailText(detail),
          census_skip ? fmt::format(",\"census\":{{\"occurrence\":{},\"swap\":{}}}", occurrence, c.bind_swap) : ""));
    if (census_skip) throw CensusSkip{};
    sink.Stop(fmt::format("{}-{}", c.name, reason), 86);
  }
};
// The shaders of the capture, as the guards and the draw records print them.
std::string ShadersRecord(const DrawCapture& c) {
  return fmt::format(
      "\"vs\":{{\"object\":{},\"hash\":\"{:016x}\",\"candidates\":{},\"cache\":\"{}\",\"fetch_words_differing\":{},"
      "\"exact\":{},\"fetch_patch\":{}}},\"ps\":{{\"object\":{},\"hash\":\"{:016x}\",\"candidates\":{},\"cache\":\"{}\"}}",
      c.vs().object, c.vs().virtual_hash, c.vs_candidates, c.vs_cached ? c.vs_cached->name : "", c.vs_diff,
      c.vs_entry ? c.vs_entry->exact : false, c.layout->patch_record, c.ps().object, c.ps().virtual_hash,
      c.ps_candidates, c.ps_cached ? c.ps_cached->name : "");
}
// The inline destination. The guards of the capture and of the translation
// stop in order with the context saved before the original commit.
class InlineGuardSink final : public GuardSink {
 public:
  explicit InlineGuardSink(PPCContext& saved) : saved_(saved) {}
  [[noreturn]] void Stop(const std::string& reason, int code) override { StopInOrder(saved_, reason, code); }
 private:
  PPCContext& saved_;
};
// The translation on the stage of the dispatcher, on by default
// (REX_NATIVE_DEFERRED_TRANSLATION=0 turns it off), fixed per session and off
// with the diagnostics that read the device and the context after the
// translation (the records outside the measurement, the START variation and
// policy, the frame trace).
bool DeferredTranslation() {
  static const bool value = [] {
    const char* text = std::getenv("REX_NATIVE_DEFERRED_TRANSLATION");
    const bool asked = !(text && std::string_view(text) == "0");
    auto set = [](const char* name) { const char* v = std::getenv(name); return v && *v; };
    const bool evidence = (Diagnostics() && MeasureSwaps() <= 0) || StartVariationMode() != 0 ||
                          set("REX_NATIVE_START_POLICY") || set("REX_NATIVE_FRAME_TRACE");
    return asked && !evidence;
  }();
  return value;
}
// The deferred destination. The guard asked the title to stop in order at
// its next own draw or Swap (RequestStop) and leaves
// the translation; the stage drops the captures after this one.
class DeferredGuardSink final : public GuardSink {
 public:
  [[noreturn]] void Stop(const std::string& reason, int code) override {
    RequestStop(reason, code);
    throw TranslationStopped{};
  }
};
// The constants of a draw the census skipped on the translator, joined to
// the next request (the inline route gives them back to the shadow instead).
ConstantCarry carry_vs, carry_ps;  // Translator only.
// A capture on the stage, translated and posted by the translator.
class CaptureTask final : public StageTask, public DrawStorageAllocated<CaptureTask> {
 public:
  CaptureTask() = default;  // Filled in place by the capture (OwnDrawBody).
  DrawCapture& capture() { return capture_; }
  bool Discardable() const override { return true; }
  void Run() override {
    profile::Timer translate_time(profile::kTranslate);
    DeferredGuardSink sink;
    JoinConstants(carry_vs, capture_.vs_constants, capture_.vs_constants_delta);
    JoinConstants(carry_ps, capture_.ps_constants, capture_.ps_constants_delta);
    try {
      TranslateDraw(capture_, sink);
    } catch (const CensusSkip&) {
      carry_vs = {std::move(capture_.vs_constants), std::move(capture_.vs_constants_delta)};
      carry_ps = {std::move(capture_.ps_constants), std::move(capture_.ps_constants_delta)};
    }
  }
 private:
  DrawCapture capture_;
};
// The quad of BeginVertices on the stage: its capture is translated into
// the build (the request kept for the take); the take, with the vertices the
// title wrote (read on the title at the take, as before), posts it.
class QuadCaptureTask final : public StageTask, public DrawStorageAllocated<QuadCaptureTask> {
 public:
  explicit QuadCaptureTask(std::shared_ptr<InlineBuild> build) : build_(std::move(build)) {}
  DrawCapture& capture() { return capture_; }  // Filled in place by the capture (OwnDrawBody).
  bool Discardable() const override { return true; }
  void Run() override {
    profile::Timer translate_time(profile::kTranslate);
    DeferredGuardSink sink;
    JoinConstants(carry_vs, capture_.vs_constants, capture_.vs_constants_delta);
    JoinConstants(carry_ps, capture_.ps_constants, capture_.ps_constants_delta);
    try {
      DrawTranslation t = TranslateDraw(capture_, sink);
      build_->request = std::move(t.request);
      build_->slot = t.quad_slot;
      build_->record = std::move(t.quad_record);
    } catch (const CensusSkip&) {
      build_->census_skipped = true;
      carry_vs = {std::move(capture_.vs_constants), std::move(capture_.vs_constants_delta)};
      carry_ps = {std::move(capture_.ps_constants), std::move(capture_.ps_constants_delta)};
    }
  }
 private:
  std::shared_ptr<InlineBuild> build_;
  DrawCapture capture_;
};
// A larger capture would bypass the pool's size classes on every draw.
static_assert(sizeof(CaptureTask) <= kDrawStorageLargestBlock && sizeof(QuadCaptureTask) <= kDrawStorageLargestBlock);
class QuadTakeTask final : public StageTask {
 public:
  QuadTakeTask(std::shared_ptr<InlineBuild> build, std::vector<uint8_t>&& vertices)
      : build_(std::move(build)), vertices_(std::move(vertices)) {}
  bool Discardable() const override { return true; }
  void Run() override;
 private:
  std::shared_ptr<InlineBuild> build_;
  std::vector<uint8_t> vertices_;
};
// The title's part of the own draw: fills the capture and leaves in saved the
// context before the original commit (the guards stop through it).
void CaptureDraw(PPCContext& ctx, uint8_t* base, bool indexed, InlineBuild* quad, DrawCapture& c, PPCContext& saved);
void OwnDrawBody(PPCContext& ctx, uint8_t* base, bool indexed, std::shared_ptr<InlineBuild> quad);
// A census skip leaves ctx as the original commit left it (the guard
// throws only after RunOverDiscard); a skipped quad has no own draw.
void OwnDraw(PPCContext& ctx, uint8_t* base, bool indexed, std::shared_ptr<InlineBuild> quad = nullptr) {
  try {
    OwnDrawBody(ctx, base, indexed, quad);
  } catch (const CensusSkip&) {
    if (quad) quad->census_skipped = true;
  }
}
void OwnDrawBody(PPCContext& ctx, uint8_t* base, bool indexed, std::shared_ptr<InlineBuild> quad) {
  const char* name = quad ? "BeginVertices-quad" : indexed ? "DrawIndexedVertices" : "DrawVertices";
  if (!device_owner || !device_owner->live() || ctx.r3.u32 != device_owner->device())
    StopInOrder(ctx, fmt::format("{}-device", name), 88);
  TakeStopRequest(ctx);  // The watchdog stop, in order on this thread.
  if (!quad) TakePendingDraw(&ctx, base, name);  // A pending quad goes first.
  sampler::RegisterThread("draw");  // REX_NATIVE_SAMPLE_PERIOD_US.
  profile::Timer draw_time(profile::kDraw);
  const uint32_t dev = ctx.r3.u32;
  PPCContext saved;
  auto* dispatcher = host::ActiveDispatcher();
  const bool deferred = dispatcher && dispatcher->Deferred();
  // Deferred, the capture is filled inside the task the stage will run
  // (no copy of it at the enqueue); inline, it lives here.
  std::unique_ptr<CaptureTask> draw_task;
  std::unique_ptr<QuadCaptureTask> quad_task;
  std::optional<DrawCapture> local;
  DrawCapture& capture = !deferred ? local.emplace()
                         : quad    ? (quad_task = std::make_unique<QuadCaptureTask>(quad))->capture()
                                   : (draw_task = std::make_unique<CaptureTask>())->capture();
  capture.name = name;
  capture.base = base;
  static std::once_flag mode_once;
  std::call_once(mode_once, [&] {
    host::Record("native-d3d-translation-mode", fmt::format("\"deferred\":{},\"asked\":{}", deferred,
                                                             DeferredTranslation()));
  });
  try {
    CaptureDraw(ctx, base, indexed, quad.get(), capture, saved);
  } catch (const CensusSkip&) {
    // The translation's copy follows every commit, skipped draws included.
    if (deferred) {
      FetchDelta delta = capture.fetch;
      dispatcher->Defer([delta = std::move(delta)] { ApplyFetch(delta); });
    } else {
      ApplyFetch(capture.fetch);
    }
    throw;
  }
  if (deferred) {  // The capture goes to the stage; the records that need the context have no route here.
    if (Diagnostics()) {
      producer_before.drawn = true;
      CountStartDraw(capture.vs_cached->name, capture.ps_cached->name);
    }
    std::unique_ptr<StageTask> task;
    if (quad_task) task = std::move(quad_task);
    else task = std::move(draw_task);
    if (!dispatcher->Enqueue(std::move(task))) StopInOrder(saved, fmt::format("{}-translation-stage-closed", name), 88);
    return;
  }
  InlineGuardSink sink(saved);
  DrawTranslation t = [&] {
    try {
      return TranslateDraw(capture, sink);
    } catch (const CensusSkip&) {
      // A skipped draw leaves its constant groups pending for the next request.
      ReturnConstants(vs_shadow, capture.vs_pending_before, capture.vs_taken_whole, capture.vs_taken_delta);
      ReturnConstants(ps_shadow, capture.ps_pending_before, capture.ps_taken_whole, capture.ps_taken_delta);
      throw;
    }
  }();
  if (quad) {  // Posted at the take, with the vertices (PostInlineQuad).
    quad->request = std::move(t.request);
    quad->slot = t.quad_slot;
    quad->record = std::move(t.quad_record);
    return;
  }
  profile::Timer evidence_time(profile::kDrawEvidence);
  const uint32_t lr = capture.lr, thread = capture.thread, primitive = capture.primitive;
  // The exact A word back after the draw (START variation).
  static const StartVariationDraw no_variation;
  static const StartMetadata no_metadata;
  static const StartLiveInputs no_live;
  std::string variation_record = RestoreStartVariation(
      saved, base, dev, capture.live ? capture.live->variation : no_variation, t.n, HostWord(vs_shadow.bytes, 3));
  if (capture.live) {  // With the START diagnostics: NoteStartDraw reads it only then.
    capture.live->policy = std::move(t.policy);
    capture.live->variation_record = std::move(variation_record);
  }
  if (t.n <= 16 && capture.want_records)
    host::Record(indexed ? "native-d3d-draw-indexed" : "native-d3d-draw-vertices", fmt::format(
        "\"draw\":{},\"lr\":{},\"guest_thread\":{},\"primitive\":{},\"base_vertex\":{},\"start_index\":{},"
        "\"start_vertex\":{},\"index_count\":{},\"index32\":{},\"indices\":[{}],\"max_index\":{},{},\"elements\":[{}],"
        "\"streams\":[{}],\"stream0_floats\":[{}],{},\"ps_key\":\"{}\",\"alpha_to_coverage\":{},\"shader_variants\":{},"
        "\"textures\":[{}],\"texture_uploads\":{},\"texture_binding_reuses\":{},\"discarded_bytes\":{},\"engine\":{}", t.n, lr,
        thread, primitive, capture.base_vertex, capture.start_index, capture.start_vertex, capture.index_count,
        t.request.index32, t.index_record, t.max_index, t.shaders_record, capture.layout->elements_record,
        t.streams_record, t.vertex_record, t.state_record, t.request.ps.key, t.request.alpha_to_coverage,
        shader_cache.variants_loaded.load(), t.textures_record, texture_uploads.load(), texture_binding_reuses.load(), capture.discarded,
        t.evidence));
  // Census of the draws with the START shaders and identification of
  // the lot; after its fifth draw the render thread stops here in order.
  if (Diagnostics()) NoteStartDraw(saved, base, dev, StartInputs{indexed, lr, thread, primitive, capture.base_vertex,
      capture.start_index, capture.index_count, t.max_index, t.request, capture.layout->stream_fetch, t.stream0,
      t.ib_descriptor, t.index_physical, t.index_endian, capture.vs(), capture.ps(), capture.vs_cached->name,
      capture.ps_cached->name, t.shaders_record, t.textures_record, t.state_record, t.evidence, t.depth_control,
      t.blend0, t.color_control, t.mode, t.vte, t.color_mask_raw & 0xF, capture.dirty_before, capture.cursor_before,
      t.n, t.fence, capture.before_metadata ? *capture.before_metadata : no_metadata,
      capture.live ? *capture.live : no_live});
}
void CaptureDraw(PPCContext& ctx, uint8_t* base, bool indexed, InlineBuild* quad, DrawCapture& c, PPCContext& saved) {
  const char* name = c.name;
  if (MeasureSwaps() < 0) StopInOrder(ctx, "native-d3d-measure-swaps-invalid", 88);
  if (MeasureSwaps() > 0 && StartVariationMode() != 0) StopInOrder(ctx, "native-d3d-measure-with-variation", 88);
  if (GuardCensus() < 0) StopInOrder(ctx, "native-d3d-guard-census-invalid", 88);
  if (GuardCensus() > 0 && MeasureSwaps() <= 0) StopInOrder(ctx, "native-d3d-guard-census-without-measure", 88);
  c.measuring = MeasureSwaps() > 0;
  // Diagnostic records outside the measurement or in a guard only, with the
  // diagnostic instruments.
  c.want_records = Diagnostics() && !c.measuring;
  const uint32_t dev = ctx.r3.u32;
  c.indexed = indexed;
  c.quad = quad != nullptr;
  c.primitive = quad ? 5 : ctx.r4.u32;
  c.lr = ctx.lr;
  c.thread = GuestThread();
  c.base_vertex = indexed ? int32_t(ctx.r5.u32) : 0;
  // The quad is drawn as a list: (0, 1, 2) and (0, 2, 3).
  c.start_index = indexed ? ctx.r6.u32 : 0;
  c.index_count = quad ? 6 : indexed ? ctx.r7.u32 : ctx.r6.u32;
  c.start_vertex = indexed || quad ? 0 : ctx.r5.u32;
  if (quad) c.quad_vertices = quad->vertices;
  if (Diagnostics() && indexed && !draw_capture_recorded.exchange(true))
    host::Record("native-d3d-draw-indexed-capture", fmt::format(
        "\"lr\":{},\"guest_thread\":{},\"primitive\":{},\"base_vertex\":{},\"start_index\":{},\"index_count\":{},"
        "\"vertex_shader\":{},\"pixel_shader\":{},\"before_original\":true", c.lr, c.thread, c.primitive, c.base_vertex,
        c.start_index, c.index_count, ShaderRecord(base, Get32(base, dev + 12688), true),
        ShaderRecord(base, Get32(base, dev + 12684), false)));
  // The own draw takes the whole D3D call, so the 16-bit index count of
  // one Xenos draw packet does not bound it (a Story Mode mesh draws 69,567
  // indices); the VGT index range (24 bits) does.
  if (c.index_count == 0 || c.index_count > 0x00FFFFFF || (!indexed && c.start_vertex > 0x00FFFFFF))
    StopInOrder(ctx, fmt::format("{}-count-uncovered", name), 86);
  // The lot's draw as the entry sees it, and the START variation written into
  // the guest state before the original commit and the copy of the request.
  const bool candidate = !c.measuring && !quad &&
                         StartEntryCandidate(base, dev, indexed, c.lr, c.primitive, c.index_count);
  if (StartVariationMode() < 0) StopInOrder(ctx, "native-d3d-start-variation-unknown", 88);
  // The START evidence lives only on the routes that read it (the records, the
  // variation, the lot's candidate): a production draw allocates none of it.
  if (candidate || c.want_records || StartVariationMode() == 1) {
    c.live = std::make_unique<StartLiveInputs>();
    c.live->candidate = candidate;
    c.before_metadata = std::make_unique<StartMetadata>();
  }
  if (StartVariationMode() == 1 && !quad) {
    StartMetadata pre;
    if (candidate) ReadStartMetadata(pre);
    c.live->variation = ApplyStartVariation(ctx, base, dev, candidate, pre, c.thread);
  }
  // Dirty masks and cursors before the original commit (START evidence).
  for (unsigned i = 0; i < 12; ++i) c.dirty_before[i] = Get32(base, dev + 4 * i);
  c.cursor_before = {Get32(base, dev + 48), Get32(base, dev + 52), Get32(base, dev + 56)};
  // The constant shadows follow the masks at +0 (VS) and +8 (PS), before
  // the original commit clears them and before any guard, so a skipped draw
  // leaves them current. The quad and the START variation refresh everything.
  {
    const bool full = quad != nullptr || StartVariationMode() == 1;
    RefreshConstants(base, dev + 0x780, c.dirty_before[0], c.dirty_before[1], full, vs_shadow);
    RefreshConstants(base, dev + 0x1780, c.dirty_before[2], c.dirty_before[3], full, ps_shadow);
    CaptureFetch(base, dev, c.dirty_before[7], full, c.fetch);  // The fetch bits of +24 (word +28).
  }
  // The before.json fields at BEFORE 821024A8 for the producer draws.
  if (c.want_records && indexed && c.lr == kStartLr) ReadStartMetadata(*c.before_metadata);
  if (candidate) ReadStartEntry(base, *c.before_metadata, *c.live);  // START entry: repeat, shaders, atlas.
  saved = ctx;
  profile::Timer original_time(profile::kDrawOriginal);
  // The quad's commit already ran inside the original BeginVertices.
  c.discarded = quad ? 0 : RunOverDiscard(ctx, base, dev, indexed ? __imp__sub_82137E00 : __imp__sub_82137A18, name,
                                          true);
  original_time.Stop();
  if (candidate) ReadStartAfter(base, *c.live);  // START entry after the original commit.
  profile::Timer shaders_time(profile::kDrawShaders);
  std::call_once(shader_cache.once, [] {  // The package of the session's API.
    narutorise::incident::Event("shader-package", "begin");
    const auto* dispatcher = host::ActiveDispatcher();
    LoadShaderCache(ShaderFormatOf(dispatcher ? dispatcher->Api() : RenderMode{}.api));
    narutorise::incident::Event("shader-package", "end", shader_cache.loaded ? "loaded" : shader_cache.error);
  });
  StartPipelinePrecompileOnce();
  InlineGuardSink sink(saved);
  const DrawGuard guard{c, sink};
  if (!shader_cache.loaded) guard("shader-cache-unavailable", fmt::format("\"error\":\"{}\"", shader_cache.error));
  // The bound shaders come from the registry filled at their birth:
  // no physical read, hash or identification per draw. An object the registry
  // misses (born before the hooks) or stale takes the per-draw path of before.
  c.vs_object = Get32(base, dev + 12688);
  c.ps_object = Get32(base, dev + 12684);
  c.vs_entry = FindShaderEntry(base, c.vs_object, true);
  c.ps_entry = FindShaderEntry(base, c.ps_object, false);
  const char* vs_why = "";
  const char* ps_why = "";
  const bool vs_read = c.vs_entry ? true : ReadBoundShader(base, c.vs_object, true, c.vs_local, &vs_why);
  const bool ps_read = c.ps_entry ? true : ReadBoundShader(base, c.ps_object, false, c.ps_local, &ps_why);
  // A null PS is the depth-only commit of 82141178 (RB_MODECONTROL at
  // +10580 low bits 5): drawn without PS and without color. The commit takes
  // the VS variant at +904 when +872 has bit 0x20; that variant stays a guard
  // (the menus use the normal one).
  c.depth_only = vs_read && !c.ps_object && (Get32(base, dev + 10580) & 7) == 5;
  // Which binding failed and why.
  if (!vs_read || (!ps_read && !c.depth_only))
    guard("shader-object-unreadable", fmt::format(
        "\"vs\":{{\"object\":{},\"read\":{},\"why\":\"{}\",\"flags872\":{},\"offset896\":{},\"offset904\":{}}},"
        "\"ps\":{{\"object\":{},\"read\":{},\"why\":\"{}\"}},\"modecontrol10580\":{}", c.vs_object, vs_read, vs_why,
        c.vs_object ? Get32(base, c.vs_object + 872) : 0, c.vs_object ? Get32(base, c.vs_object + 896) : 0,
        c.vs_object ? Get32(base, c.vs_object + 904) : 0, c.ps_object, ps_read, ps_why, Get32(base, dev + 10580)));
  if (c.depth_only && (Get32(base, c.vs_object + 872) & 0x20))
    guard("depth-only-vs-variant-uncovered", fmt::format("\"vs\":{{\"object\":{},\"flags872\":{},\"offset904\":{}}}",
                                                          c.vs_object, Get32(base, c.vs_object + 872),
                                                          Get32(base, c.vs_object + 904)));
  // Identification, exact and once per entry; a PS without entry as before.
  if (c.vs_entry) {
    IdentifyEntry(*c.vs_entry);
    c.vs_cached = c.vs_entry->cached;
    c.vs_candidates = c.vs_entry->candidates;
    c.vs_diff = c.vs_entry->differing;
  }
  // A depth-only draw has no PS; an empty entry (no name, no textures)
  // stands for it below.
  static CachedShader no_pixel_shader;
  c.ps_cached = c.depth_only ? &no_pixel_shader : nullptr;
  if (!c.depth_only) {
    if (c.ps_entry) {
      IdentifyEntry(*c.ps_entry);
      c.ps_cached = c.ps_entry->cached;
      c.ps_candidates = c.ps_entry->candidates;
      c.ps_diff = c.ps_entry->differing;
    } else {
      c.ps_cached = IdentifyShader(c.ps(), false, c.ps_candidates, c.ps_diff);
    }
  }
  // The vertex input of the pair (VS, declaration, strides), translated once.
  c.layout = FindOrBuildVertexLayout(saved, base, dev, c.vs_entry.get(), c.vs(), quad != nullptr, c.vs_cached,
                                     c.vs_candidates, c.vs_diff);
  shaders_time.Stop();
  const auto shaders_record = MakeLazyRecord([&] { return ShadersRecord(c); });
  if (!c.layout->ok) guard(c.layout->reason, shaders_record + "," + c.layout->detail);
  if (!c.vs_cached || !c.ps_cached) {
    // The container and the microcode of an unidentified shader go to
    // REX_NATIVE_FRAME_DIR (shader-vs|ps-<virtual hash>.bin, container then
    // code, guest bytes), once per hash, to be compared offline with the cache.
    auto dump = [&](const BoundShader& s, bool vertex) {
      const char* dir = std::getenv("REX_NATIVE_FRAME_DIR");
      if (!dir || !*dir) return std::string("null");
      const std::string file = fmt::format("{}/shader-{}-{:016x}.bin", dir, vertex ? "vs" : "ps", s.virtual_hash);
      std::vector<uint8_t> raw(s.code_size);
      const bool code_read = ReadPhysical(base, s.code_physical, s.code_size, raw.data());
      if (std::FILE* existing = std::fopen(file.c_str(), "rb")) { std::fclose(existing); return std::string("\"exists\""); }
      std::FILE* out = std::fopen(file.c_str(), "wb");
      if (!out) return std::string("\"write-failed\"");
      std::fwrite(base + s.container, 1, s.virtual_size, out);
      if (code_read) std::fwrite(raw.data(), 1, raw.size(), out);
      std::fclose(out);
      return std::string("\"written\"");
    };
    guard("shader-not-identified", shaders_record + fmt::format(
        ",\"vs_sizes\":[{},{}],\"ps_sizes\":[{},{}],\"vs_dump\":{},\"ps_dump\":{}", c.vs().virtual_size, c.vs().code_size,
        c.ps().virtual_size, c.ps().code_size, c.vs_cached ? "null" : dump(c.vs(), true),
        c.ps_cached ? "null" : dump(c.ps(), false)));
  }
  // The sampler table of the PS, read once per entry (guest object).
  if (c.ps_cached->textures) {
    if (c.ps_entry) {
      if (!c.ps_entry->samplers_read) {
        c.ps_entry->samplers_ok = ReadSamplers(base, c.ps(), c.ps_entry->samplers);
        c.ps_entry->samplers_read = true;
      }
      c.samplers_ok = c.ps_entry->samplers_ok;
    } else {
      c.samplers_ok = ReadSamplers(base, c.ps(), c.samplers_local);
    }
  }
  // Registers after the original commit: the blocks, the bound headers with
  // the EDRAM base of each, the index buffer with its first word and its
  // base, the enabled user clip planes (PA_CL_UCP_n at +10272 + 16n, not
  // in the state snapshot; a disabled plane stays zero).
  ReadStateRegisters(base, dev, c.registers);
  for (unsigned i = 0; i < 5; ++i) {
    c.targets[i] = Get32(base, dev + 12432 + 4 * i);
    c.target_edram[i] = c.targets[i] ? Get32(base, c.targets[i] + 0x1C) & 0xFFF : 0;
  }
  c.color_backing = Backing(c.targets[0]);  // The tiling backings of the moment of the draw.
  c.depth_backing = Backing(c.targets[4]);
  if (indexed) {
    c.ib = Get32(base, dev + 12428);
    if (c.ib) {
      c.ib_common = Get32(base, c.ib);
      c.ib_base = Get32(base, c.ib + 0x18);
    }
  }
  if (const uint32_t planes = StateRegister(c.registers, 0x2204) & 0x3F) {
    for (unsigned p = 0; p < 6; ++p) {
      if (!((planes >> p) & 1)) continue;
      for (unsigned k = 0; k < 4; ++k) {
        const uint32_t v = Get32(base, dev + 10272 + 16 * p + 4 * k);
        std::memcpy(&c.clip_planes[4 * p + k], &v, 4);
      }
    }
  }
  if (TextureVerify()) c.fetch_missed = FetchMissedWrites(base, dev);
  // Only the draws whose record needs the engine evidence wait for
  // it: the first 16 and, outside the measurement, the draws with the START shaders.
  c.evidence_draw = c.want_records && (own_draws.load() < 16 || (c.vs_cached->name == "vs-00055311" &&
                                                                 c.ps_cached->name == "ps-002C60A9"));
  // On the stage the draw is numbered here, so the Swap counts the frame's
  // captures; inline it is numbered at the post, after every guard.
  if (auto* dispatcher = host::ActiveDispatcher(); dispatcher && dispatcher->Deferred()) c.n = own_draws.fetch_add(1) + 1;
  // The constants the request carries, taken after the guards of the
  // capture: the whole blocks for the quad, the evidence draws (the START
  // lot's identity and copies need them) and the START variation, the pending
  // groups for the batched draws. A draw the translation skips gives them back
  // (ReturnConstants).
  const bool whole = quad != nullptr || c.evidence_draw || candidate || StartVariationMode() == 1;
  c.vs_constants_version = vs_shadow.version;
  c.ps_constants_version = ps_shadow.version;
  c.vs_pending_before = vs_shadow.pending;
  c.ps_pending_before = ps_shadow.pending;
  TakeConstants(vs_shadow, whole, c.vs_constants, c.vs_constants_delta);
  TakeConstants(ps_shadow, whole, c.ps_constants, c.ps_constants_delta);
  c.vs_taken_whole = !c.vs_constants.empty();
  c.ps_taken_whole = !c.ps_constants.empty();
  c.vs_taken_delta = c.vs_constants_delta.size();
  c.ps_taken_delta = c.ps_constants_delta.size();
  if (FrameTraceActive()) {  // The whole shadows, for the diagnosis of the traced draws.
    c.trace_vs_constants.assign(vs_shadow.bytes.begin(), vs_shadow.bytes.end());
    c.trace_ps_constants.assign(ps_shadow.bytes.begin(), ps_shadow.bytes.end());
  } else if (ProbeTexture() && vs_shadow.bytes.size() >= 64) {  // The rows the probe records.
    c.trace_vs_constants.assign(vs_shadow.bytes.begin(), vs_shadow.bytes.begin() + 64);
  }
  c.bind_swap = swaps.load() + 1;
}
DrawTranslation TranslateDraw(DrawCapture& c, GuardSink& sink) {
  ApplyFetch(c.fetch);  // The copy of the fetch constants follows the commit.
  DrawTranslation t;
  const DrawGuard guard{c, sink};
  uint8_t* base = c.base;
  const char* name = c.name;
  const bool indexed = c.indexed, quad = c.quad;
  const uint32_t lr = c.lr, thread = c.thread, index_count = c.index_count, start_index = c.start_index;
  const uint32_t start_vertex = c.start_vertex;
  const int32_t base_vertex = c.base_vertex;
  const bool want_records = c.want_records;
  const BoundShader& vs = c.vs();
  const BoundShader& ps = c.ps();
  CachedShader* vs_cached = c.vs_cached;
  CachedShader* ps_cached = c.ps_cached;
  const VertexLayout& layout = *c.layout;
  const std::string& elements_record = layout.elements_record;
  const std::vector<uint32_t>& stream_fetch = layout.stream_fetch;
  const bool depth_only = c.depth_only;
  const auto shaders_record = MakeLazyRecord([&] { return ShadersRecord(c); });
  DrawRequest& request = t.request;  // Built in place: the result carries it.
  request.elements = layout.shared_elements;  // Shared, with the layout id of the pipeline key.
  request.layout_id = layout.elements_id;
  request.streams.resize(layout.stream_strides.size());
  for (size_t i = 0; i < layout.stream_strides.size(); ++i) request.streams[i].stride = layout.stream_strides[i];
  // Textures of the pixel shader and of the vertex shader,
  // the fetch constants 16 to 19 of its tfetch instructions (its sampler table
  // counts them from 0), 1D in the Story Mode scene (bone matrices).
  // The draw binds the translated texture of each slot (BindTexture).
  std::string textures_record;
  auto texture_guard = [&](const std::string& reason, uint32_t reg, bool vertex) {
    guard(reason, MakeLazyRecord([&] {
      return shaders_record + ",\"texture\":{" + TextureRecord(base, reg, vertex) + "}," + TexturesRecord(base, vs, ps);
    }));
  };
  auto fetch_missed = [&](uint32_t reg) { return reg < 32 && ((c.fetch_missed >> reg) & 1); };
  if (vs_cached->textures) {
    if (vs_cached->fetch_constants.empty())
      guard("vertex-textures-uncovered", MakeLazyRecord([&] { return shaders_record + "," + TexturesRecord(base, vs, ps); }));
    for (const auto& [constant, fetch_dimension] : vs_cached->fetch_constants) {
      if (fetch_dimension != 0)
        guard("vertex-texture-dimension-uncovered", shaders_record + fmt::format(",\"constant\":{},\"fetch_dimension\":{}",
                                                                                 constant, fetch_dimension));
      ShaderSampler sampler;
      sampler.reg = constant;
      sampler.type = 11;  // D3DXPT_SAMPLER1D
      sampler.name = fmt::format("vs-s{}", constant);
      std::string reason;
      if (!BindTexture(base, sampler, true, fetch_missed(sampler.reg), request.textures.emplace_back(), reason))
        texture_guard(reason, sampler.reg, true);
      if (want_records) textures_record += fmt::format("{}{{\"name\":\"{}\",{}}}", textures_record.empty() ? "" : ",",
                                                       sampler.name, TextureRecord(base, sampler.reg, true));
    }
  }
  if (ps_cached->tfetch_override)  // Filters of the instruction instead of the fetch constant.
    guard(fmt::format("uncovered-tfetch.{}", ps_cached->tfetch_override), shaders_record);
  if (ps_cached->textures) {
    profile::Timer textures_time(profile::kDrawTextures);
    const std::vector<ShaderSampler>& samplers = c.ps_samplers();
    if (!c.samplers_ok || samplers.empty())
      guard("sampler-table-uncovered", MakeLazyRecord([&] { return shaders_record + "," + TexturesRecord(base, vs, ps); }));
    request.textures.reserve(request.textures.size() + samplers.size());
    for (const auto& sampler : samplers) {
      std::string reason;
      if (!BindTexture(base, sampler, false, fetch_missed(sampler.reg), request.textures.emplace_back(), reason))
        texture_guard(reason, sampler.reg, false);
      if (want_records) textures_record += fmt::format("{}{{\"name\":\"{}\",{}}}", textures_record.empty() ? "" : ",",
                                                       sampler.name, TextureRecord(base, sampler.reg, false));
    }
  }
  // The VS takes its base variant (spec 0: its vertex fetch is decoded by the
  // descriptors); the PS variant is chosen with the alpha test state below.
  request.vs.key = vs_cached->name;
  request.vs.format = request.ps.format = shader_cache.format;
  request.vs.spec_mask = vs_cached->spec_mask;
  profile::Timer load_time(profile::kDrawShaderLoad);
  request.vs.bytes = LoadModule(*vs_cached);
  load_time.Stop();
  if (!request.vs.bytes) guard("vertex-shader-unavailable", shaders_record);
  // Indices: the bound index buffer, or start .. start + count - 1 for DrawVertices.
  // The draw's range of the index buffer is a buffer entry (BindBuffer):
  // bound on the GPU once hosted, read inline while on probation.
  profile::Timer streams_time(profile::kDrawStreams);
  request.index_count = index_count;
  const uint64_t bind_swap = c.bind_swap;
  uint32_t max_index = 0;
  if (indexed) {
    const uint32_t ib = c.ib;
    if (!ib) guard("index-buffer-unbound", shaders_record);
    const uint32_t ib_common = c.ib_common;
    request.index32 = (ib_common >> 31) & 1;
    t.index_endian = (ib_common >> 29) & 3;
    const uint32_t ib_physical = GuestPhysicalAlias(c.ib_base) + start_index * (request.index32 ? 4 : 2);
    t.ib_descriptor = ib;  // START evidence.
    t.index_physical = ib_physical;
    if (request.index32 ? t.index_endian != 2 : t.index_endian != 1)
      guard("index-buffer-uncovered", fmt::format("\"common\":{},\"physical\":{}", ib_common, ib_physical));
    const BufferKey key{ib_physical, index_count * (request.index32 ? 4u : 2u), uint8_t(t.index_endian), 1,
                        uint8_t(request.index32)};
    BufferUse use;
    std::string why;
    if (!BindBuffer(base, key, key.bytes, bind_swap, use, why))
      guard("index-buffer-uncovered", fmt::format("\"common\":{},\"physical\":{},\"why\":\"{}\"", ib_common,
                                                  ib_physical, why));
    max_index = use.max_index;
    if (use.hosted) {
      request.index_key = use.key;
      request.index_offset = use.offset;
    } else {
      request.indices = std::move(use.data);
    }
  } else if (quad) {
    constexpr std::array<uint16_t, 6> fan{0, 1, 2, 0, 2, 3};
    request.index32 = false;
    request.indices.resize(fan.size() * 2);
    std::memcpy(request.indices.data(), fan.data(), request.indices.size());
  } else {
    request.index32 = start_vertex + index_count > 0xFFFF;
    request.indices.resize(size_t(index_count) * (request.index32 ? 4 : 2));
    for (uint32_t i = 0; i < index_count; ++i) {
      const uint32_t v = start_vertex + i;
      std::memcpy(&request.indices[size_t(i) * (request.index32 ? 4 : 2)], &v, request.index32 ? 4 : 2);
    }
  }
  const uint32_t index_size = request.index32 ? 4 : 2;
  if (!indexed) {  // The inline indices of the quad and of DrawVertices.
    for (uint32_t i = 0; i < index_count; ++i) {
      uint32_t v = 0;
      std::memcpy(&v, &request.indices[size_t(i) * index_size], index_size);
      max_index = std::max(max_index, v);
    }
  }
  const auto index_record = MakeLazyRecord([&] {  // The first 12 inline indices, on use.
    std::string out;
    for (uint32_t i = 0; i < std::min<uint32_t>(index_count, 12) && size_t(i + 1) * index_size <= request.indices.size(); ++i) {
      uint32_t v = 0;
      std::memcpy(&v, &request.indices[size_t(i) * index_size], index_size);
      out += fmt::format("{}{}", i ? "," : "", v);
    }
    return out;
  });
  // Vertex streams from the fetch constants, the copy of the translation
  // (fetch f: slot f / 3, words 2 (f % 3) and the next). Each (address,
  // bytes, endian) is a buffer entry (BindBuffer): bound whole on the GPU once
  // hosted, read inline (the needed bytes) while on probation. The record of
  // the streams is formatted only when a guard or a record takes it, and the
  // per-draw lists keep their storage.
  struct StreamNote {
    uint32_t fetch, address, bytes, endian, stride;
    uint64_t read;
    int hosted;  // -1 before the binding (the record closes without the field).
  };
  thread_local std::vector<StreamNote> stream_notes;
  thread_local std::vector<uint32_t> stream_endian, stream_address;
  stream_notes.clear();
  stream_endian.clear();
  stream_address.clear();
  auto streams_text = [&] {
    std::string out;
    for (size_t i = 0; i < stream_notes.size(); ++i) {
      const StreamNote& n = stream_notes[i];
      out += fmt::format("{}{{\"fetch\":{},\"address\":{},\"bytes\":{},\"endian\":{},\"stride\":{},\"read\":{}{}}}",
                         i ? "," : "", n.fetch, n.address, n.bytes, n.endian, n.stride, n.read,
                         n.hosted < 0 ? "" : n.hosted ? ",\"hosted\":true" : ",\"hosted\":false");
    }
    return out;
  };
  std::array<uint32_t, 3> stream0{};  // Address, bytes and endian of slot 0.
  for (size_t slot = 0; slot < stream_fetch.size(); ++slot) {
    const uint32_t f = stream_fetch[slot];
    const std::array<uint32_t, 6>& words = FetchConstantSlot(f / 3);
    // The quad's fetch 95 is the inline constant of the packet, on the
    // own vertex area; its bytes are read at the take, once the title wrote them.
    const bool inline_fetch = quad && f == InlineBuild::kFetch;
    const uint32_t d0 = inline_fetch ? GuestPhysicalAlias(c.quad_vertices) | 3 : words[2 * (f % 3)];
    const uint32_t d1 = inline_fetch ? (InlineBuild::kBytes / 4) << 2 | InlineBuild::kEndian : words[2 * (f % 3) + 1];
    const uint32_t address = d0 & ~3u, size = ((d1 >> 2) & 0xFFFFFF) * 4, endian = d1 & 3;
    if (!slot) stream0 = {address, size, endian};
    stream_endian.push_back(endian);
    stream_address.push_back(address);
    auto& stream = request.streams[slot];
    const uint64_t needed = uint64_t(int64_t(max_index) + base_vertex + 1) * stream.stride;
    StreamNote& note = stream_notes.emplace_back(StreamNote{f, address, size, endian, stream.stride, needed, -1});
    if ((d0 & 3) != 3 || int64_t(max_index) + base_vertex < 0 || needed > size || needed > 0x400000 ||
        size > kBufferBytesMax) {
      guard("vertex-stream-uncovered", fmt::format(
          "\"streams\":[{}],{},\"elements\":[{}],\"start_index\":{},\"max_index\":{},\"indices\":[{}]",
          streams_text(), shaders_record.get(), elements_record, start_index, max_index, index_record.get()));
    }
    if (inline_fetch) {
      stream.data.resize(size_t(needed));
      t.quad_slot = uint32_t(slot);
      note.hosted = 0;
      continue;
    }
    const BufferKey key{address, size, uint8_t(endian), 0, 0};
    BufferUse use;
    std::string why;
    if (!BindBuffer(base, key, uint32_t(needed), bind_swap, use, why))
      guard("vertex-stream-unreadable", fmt::format("\"streams\":[{}],\"why\":\"{}\"", streams_text(), why));
    note.hosted = use.hosted ? 1 : 0;
    if (use.hosted) {
      stream.key = use.key;
      stream.offset = use.offset;
      stream.bytes = use.bytes;
    } else {
      stream.data = std::move(use.data);
    }
  }
  if (quad && t.quad_slot == UINT32_MAX) guard("inline-fetch-unused", fmt::format("\"streams\":[{}]", streams_text()));
  // The descriptor of each element (vfetchNaruto) at c20..c23 of the
  // shared constants, the streams as read: no expansion on the CPU. A 16-bit
  // pair after the 8in32 swap arrives as (Y, X) and takes the 8in32
  // descriptor; 8in16 keeps the order. The template shaders of the START
  // policy (a) read g_SwappedTexcoords (shared +260) instead.
  uint32_t swapped_texcoords = 0;
  for (size_t i = 0; i < layout.fetch.size(); ++i) {
    const VertexFetch& f = layout.fetch[i];
    const uint32_t endian = stream_endian[f.slot];
    if (f.by_endian && endian != 1 && endian != 2)
      guard("vertex-pair-endian-uncovered", fmt::format("\"streams\":[{}]", streams_text()));
    const uint32_t descriptor = f.by_endian && endian == 2 ? f.descriptor_8in32 : f.descriptor;
    std::memcpy(&request.shared_constants[320 + 4 * i], &descriptor, 4);
    if (f.texcoord_index >= 0 && endian == 2) swapped_texcoords |= 1u << f.texcoord_index;
  }
  std::memcpy(&request.shared_constants[260], &swapped_texcoords, 4);
  streams_time.Stop();
  request.base_vertex = base_vertex;
  if (c.primitive == 4 || quad) request.topology = Topology::kTriangleList;  // The quad as a list.
  else if (c.primitive == 6) request.topology = Topology::kTriangleStrip;
  else guard("primitive-uncovered", shaders_record);
  // Constants: from the shadows, with their versions, as the capture took them.
  request.vs_constants_version = c.vs_constants_version;
  request.ps_constants_version = c.ps_constants_version;
  // Register blocks of the capture: reg(n) by the register number.
  auto reg = [&](uint32_t n) { return StateRegister(c.registers, n); };
  auto regf = [&](uint32_t n) { const uint32_t v = reg(n); float f; std::memcpy(&f, &v, 4); return f; };
  StateShadow& s = state_shadow;
  const auto state_record = MakeLazyRecord([&] { return fmt::format(
      "\"depthcontrol\":{},\"blendcontrol0\":{},\"colorcontrol\":{},\"mode_cntl\":{},\"vte_cntl\":{},\"color_mask\":{},"
      "\"alpha_ref\":{},\"stencilrefmask\":{},\"stencilrefmask_bf\":{}", s.depth_control, s.blend0, s.color_control,
      s.mode, s.vte, s.color_mask_raw, JsonFloat(s.alpha_ref), s.refmask, s.refmask_bf); });
  // The translated state follows a value snapshot of its registers and
  // the bindings it depends on; otherwise the shadow is applied. A guard inside
  // the refresh leaves the shadow invalid, so the next draw translates (and
  // guards) again, as before.
  request.depth_only = depth_only;
  StateKey state_key;
  state_key.targets = c.targets;
  state_key.depth_only = depth_only;
  state_key.topology = request.topology;
  if (!s.valid || !(state_key == s.key) || c.registers != s.registers) {
    s.valid = false;
    ++state_refreshes;
    s.registers = c.registers;
    s.depth_control = reg(0x2200);
    s.color_control = reg(0x2202);
    s.mode = reg(0x2205);
    s.vte = reg(0x2206);
    s.blend0 = reg(0x2201);
    s.color_mask_raw = reg(0x2104);
    s.refmask = reg(0x210D);
    s.refmask_bf = reg(0x210C);
    s.alpha_ref = regf(0x210E);  // RB_ALPHA_REF
    const uint32_t depth_control = s.depth_control, color_control = s.color_control, mode = s.mode, vte = s.vte;
    // Stencil by RB_DEPTHCONTROL (enable bit 0; front
    // function, fail, zpass and zfail from bit 8; the back face from bit 20 when
    // bit 7 is set, otherwise the front one) and RB_STENCILREFMASK (0x210D: ref,
    // read mask, write mask). The PSO has one ref and one pair of masks, so a back
    // face whose RB_STENCILREFMASK_BF (0x210C) differs stays a guard.
    request.stencil_enable = depth_control & 1;
    if (request.stencil_enable) {
      const uint32_t refmask = s.refmask;
      const bool back_face = (depth_control >> 7) & 1;
      if (back_face && (s.refmask_bf & 0xFFFFFF) != (refmask & 0xFFFFFF)) guard("stencil-back-refmask-uncovered", state_record);
      request.stencil_front = (depth_control >> 8) & 0xFFF;
      request.stencil_back = back_face ? (depth_control >> 20) & 0xFFF : request.stencil_front;
      request.stencil_ref = refmask & 0xFF;
      request.stencil_read_mask = (refmask >> 8) & 0xFF;
      request.stencil_write_mask = (refmask >> 16) & 0xFF;
    } else {
      request.stencil_front = request.stencil_back = 7;
      request.stencil_ref = 0;
      request.stencil_read_mask = request.stencil_write_mask = 0xFF;
    }
    if ((vte & 0x3F) != 0x3F) guard("viewport-transform-uncovered", state_record);
    // RB_COLORCONTROL alpha test (bits 0..3) by the
    // SPEC_CONSTANT_ALPHA_TEST variant of the PS, clip(oC0.w - g_AlphaThreshold)
    // with RB_ALPHA_REF at shared +272: it passes alpha >= threshold, so GREATER
    // and GEQUAL are covered (they differ only at equality); ALWAYS needs no
    // test. Alpha-to-mask (bit 4) becomes alpha to coverage, without the
    // dither offsets of bits 24..31.
    s.ps_spec = 0;
    if (color_control & 8) {
      const uint32_t func = color_control & 7;
      if (func == 4 || func == 6) s.ps_spec = 2;  // SPEC_CONSTANT_ALPHA_TEST
      else if (func != 7) guard("alpha-func-uncovered", state_record);
    }
    if (s.ps_spec && !std::isfinite(s.alpha_ref)) guard("alpha-ref-uncovered", state_record);
    request.alpha_to_coverage = (color_control >> 4) & 1;
    request.blend_control = s.blend0;
    request.color_mask = s.color_mask_raw & 0xF;
    for (unsigned i = 0; i < 4; ++i) request.blend_factor[i] = regf(0x2105 + i);
    request.depth_enable = (depth_control >> 1) & 1;
    request.depth_write = request.depth_enable && ((depth_control >> 2) & 1);
    request.depth_func = (depth_control >> 4) & 7;
    request.cull_front = mode & 1;
    request.cull_back = (mode >> 1) & 1;
    request.front_ccw = ((mode >> 2) & 1) == 0;
    // Polygon offset, as the SDK's non-ROV path (GetPreferredFacePolygonOffset
    // and GetD3D10IntegerPolygonOffset): the front offset unless that face is off
    // or culled, then the back one; one bias for both faces. PA_SU_POLY_OFFSET_*
    // (0x2380..0x2383) live at +10832..+10844 (D3DRS_SLOPESCALEDEPTHBIAS is
    // +10832 / 16 and D3DRS_DEPTHBIAS +10836 in the GetRenderState table).
    {
      auto polyf = [&](uint32_t n) { return regf(0x2380 + n); };
      float scale = 0, offset = 0;
      if (((mode >> 11) & 1) && !request.cull_front) { scale = polyf(0); offset = polyf(1); }
      if (((mode >> 12) & 1) && !request.cull_back && !scale && !offset) { scale = polyf(2); offset = polyf(3); }
      if (!std::isfinite(scale) || !std::isfinite(offset)) guard("polygon-offset-not-finite", state_record);
      const bool float24 = (reg(0x2002) >> 16) & 1;  // RB_DEPTH_INFO.depth_format: k_24_8_FLOAT
      int32_t bias = int32_t(std::ceil(std::abs(offset) * (float24 ? float(1u << 24) / 8.0f : float((1u << 24) - 1))));
      if (float24) bias <<= 3;
      request.depth_bias = offset < 0 ? -bias : bias;
      request.depth_bias_slope = scale * (1.0f / 16.0f);  // xenos::kPolygonOffsetScaleSubpixelUnit
    }
    const float xscale = regf(0x210F), xoffset = regf(0x2110), yscale = regf(0x2111), yoffset = regf(0x2112);
    const float zscale = regf(0x2113), zoffset = regf(0x2114);
    const float width = 2 * std::abs(xscale), height = 2 * std::abs(yscale);
    request.viewport = {xoffset - std::abs(xscale), yoffset - std::abs(yscale), width, height, zoffset, zoffset + zscale};
    const uint32_t tl = reg(0x2011), br = reg(0x2012);
    request.scissor = {int32_t(tl & 0x7FFF), int32_t((tl >> 16) & 0x7FFF), int32_t(br & 0x7FFF), int32_t((br >> 16) & 0x7FFF)};
    // Shared constants: booleans unused here, texcoord swaps above; half-pixel
    // offset (1/W, -1/H) of the viewport.
    s.half[0] = width > 0 ? 1.0f / width : 0.0f;
    s.half[1] = height > 0 ? -1.0f / height : 0.0f;
    // Per-register coverage of the fields the draw does not translate. A
    // field outside its covered value (the value of every START draw) stops
    // with the register and field name, below.
    s.uncovered_field = UncoveredDrawField(c, reg, request, mode, vte, color_control);
    s.clip_planes = reg(0x2204) & 0x3F;
    CaptureState(request, s);
    s.key = state_key;
    s.valid = true;
  } else {
    ++state_reuses;
    ApplyState(s, request);
  }
  const uint32_t depth_control = s.depth_control, color_control = s.color_control, mode = s.mode, vte = s.vte;
  const float alpha_ref = s.alpha_ref;
  // Transparency antialiasing in multisampled targets (TransparencySpec);
  // the threshold at +272 serves the alpha to coverage variant too.
  uint32_t ps_spec = s.ps_spec;
  if (ps_spec && !depth_only) {
    ps_spec = TransparencySpec(ps_spec, ps_cached->spec_mask, resources.Samples(c.targets[0]), request.blend_control);
    if (ps_spec != s.ps_spec) request.alpha_to_coverage = true;
  }
  std::memcpy(&request.shared_constants[264], s.half, 8);
  std::memcpy(&request.shared_constants[272], &alpha_ref, 4);
  // The user clip planes, only in the draws that enable one, as the
  // capture read them (PA_CL_UCP_n; a disabled plane stays (0, 0, 0, 0) at
  // shared +384 + 16n and clips nothing).
  if (s.clip_planes) {
    const std::array<float, 24>& planes = c.clip_planes;
    std::memcpy(&request.shared_constants[384], planes.data(), sizeof(planes));
    profile::Count(profile::kClipPlaneDraws, 1);
    const auto planes_record = MakeLazyRecord([&] {
      std::string out;
      for (unsigned i = 0; i < 24; ++i) out += fmt::format("{}{}", i ? "," : "", JsonFloat(planes[i]));
      return fmt::format("{},\"clip_cntl\":{},\"clip_planes\":[{}],\"target\":{},\"viewport\":[{}]", state_record.get(),
                         reg(0x2204), out, c.targets[0], fmt::join(request.viewport, ","));
    });
    for (float f : planes)
      if (!std::isfinite(f)) guard("clip-plane-not-finite", planes_record);
    if (Diagnostics() && clip_plane_records.fetch_add(1) < 8) host::Record("native-d3d-clip-planes",
        fmt::format("\"lr\":{},\"guest_thread\":{},{}", lr, thread, planes_record.get()));
  }
  if (s.uncovered_field) {
    guard(fmt::format("uncovered-{}", s.uncovered_field), state_record + fmt::format(
        ",\"field\":\"{}\",\"clip_cntl\":{},\"modecontrol\":{},\"vtx_cntl\":{},\"window_offset\":{},"
        "\"viewport\":[{}],\"render_targets\":[{},{},{},{}],\"samples\":{},\"blend_factor\":[{}]", s.uncovered_field,
        reg(0x2204), reg(0x2208), reg(0x2302), reg(0x2010), fmt::join(request.viewport, ","), c.targets[0],
        c.targets[1], c.targets[2], c.targets[3], resources.Samples(c.targets[0]), fmt::join(request.blend_factor, ",")));
  }
  std::string link_error;
  if (depth_only) {
    request.ps.key = "depth-only";  // No PS, no color written (the alpha test has no PS to act on).
  } else {
    {
      profile::Timer ps_load_time(profile::kDrawShaderLoad);
      request.ps.bytes = LoadShader(*ps_cached, ps_spec, link_error);
    }
    if (!request.ps.bytes)
      guard("pixel-shader-unavailable", shaders_record + fmt::format(",\"spec\":{},\"error\":\"{}\"", ps_spec,
                                                                     JsonText(link_error)));
    request.ps.key = ps_spec & ps_cached->spec_mask ? fmt::format("{}+spec{}", ps_cached->name, ps_spec)
                                                    : ps_cached->name;
    request.ps.spec_mask = ps_cached->spec_mask;
    request.ps.spec_value = ps_spec;
  }
  // Policy (a) of the START lot, selected by the current microcode.
  std::call_once(start_policy.once, LoadStartPolicy);
  if (start_policy.requested && vs_cached->name == start_policy.vs_cache && ps_cached->name == start_policy.ps_cache) {
    if (!start_policy.loaded) guard("start-policy-unavailable", "\"policy\":" + StartPolicyJson());
    const auto vs_patched = BigEndianBytes(layout.patched_code.data(), layout.patched_code.size());
    const auto vs_physical = StartPhysical(base, vs.code_physical, vs.code_size);
    const auto ps_physical = StartPhysical(base, ps.code_physical, ps.code_size);
    const std::string vs_patched_sha = Sha256Hex(vs_patched.data(), vs_patched.size());
    const std::string vs_physical_sha = Sha256Hex(vs_physical.data(), vs_physical.size());
    const std::string ps_physical_sha = Sha256Hex(ps_physical.data(), ps_physical.size());
    const bool selected = !ps_spec && vs_patched_sha == start_policy.vs_ucode &&
                          vs_physical_sha == start_policy.vs_ucode && ps_physical_sha == start_policy.ps_ucode;
    t.policy = fmt::format("{{\"policy\":\"{}\",\"policy_file_sha256\":\"{}\",\"selected\":{},\"vs_patched_sha256\":\"{}\","
                           "\"vs_physical_sha256\":\"{}\",\"ps_physical_sha256\":\"{}\",\"ps_spec\":{},"
                           "\"vs_dxil_sha256\":\"{}\",\"ps_dxil_sha256\":\"{}\"}}", start_policy.name,
                           start_policy.file_sha, selected, vs_patched_sha, vs_physical_sha, ps_physical_sha, ps_spec,
                           start_policy.vs_dxil_sha, start_policy.ps_dxil_sha);
    if (!selected) guard("start-policy-microcode-differs", "\"policy\":" + t.policy);
    // The template shaders of the policy, unspecialized.
    request.vs = ShaderRef{fmt::format("{}:{}", start_policy.name, vs_cached->name), ShaderStage::kVertex,
                           ShaderFormat::kDxil, start_policy.vs_dxil};
    request.ps = ShaderRef{fmt::format("{}:{}", start_policy.name, ps_cached->name), ShaderStage::kPixel,
                           ShaderFormat::kDxil, start_policy.ps_dxil};
    // The template shaders of the policy read the typed formats.
    if (!layout.legacy_ok) guard("start-policy-vertex-format-uncovered", fmt::format("\"elements\":[{}]", elements_record));
    request.elements = layout.shared_legacy_elements;
    request.layout_id = layout.legacy_elements_id;
    if (start_policy.selections.fetch_add(1) == 0)
      host::Record("native-d3d-start-policy-selected", fmt::format(
          "\"lr\":{},\"guest_thread\":{},\"selection\":{},\"policy\":{}", lr, thread, t.policy, StartPolicyJson()));
  }
  // A depth-only draw writes no color, so it binds the depth target only.
  request.color_key = depth_only ? 0 : c.color_backing;
  request.depth_key = request.depth_enable || request.stencil_enable ? c.depth_backing : 0;
  if (!request.color_key && !depth_only) guard("render-target-unbound", state_record);
  if (depth_only && !request.depth_key) guard("depth-only-target-unbound", state_record);
  if (const uint32_t probe = ProbeTexture();
      probe && std::any_of(request.textures.begin(), request.textures.end(), [&](const auto& t) { return t.key == probe; }) &&
      probe_draws.fetch_add(1) < kProbeDraws) {
    // The positions as the shader fetches them (three floats, swapped as the stream).
    const std::vector<VertexElement>& elements = *request.elements;
    const auto position = std::find_if(elements.begin(), elements.end(), [](const VertexElement& e) {
      return std::string_view(e.semantic) == "POSITION" && e.semantic_index == 0;
    });
    std::string positions = "null";
    if (position != elements.end() && position->slot < stream_address.size()) {
      const VertexStream& stream = request.streams[position->slot];
      std::vector<uint8_t> raw(12);
      positions.clear();
      for (uint32_t v = 0; v < 4; ++v) {
        const int64_t at = (int64_t(v) + base_vertex) * stream.stride + position->offset;
        float f[3]{};
        if (at >= 0 && size_t(at) + 12 <= stream.data.size()) {
          std::memcpy(f, &stream.data[size_t(at)], 12);
        } else if (at >= 0 && stream.key && ReadPhysical(base, stream_address[position->slot] + uint32_t(at), 12, raw.data()) &&
                   SwapEndian(raw, stream_endian[position->slot])) {
          std::memcpy(f, raw.data(), 12);
        }
        positions += fmt::format("{}{},{},{}", positions.empty() ? "" : ",", JsonFloat(f[0]), JsonFloat(f[1]), JsonFloat(f[2]));
      }
      positions = "[" + positions + "]";
    }
    float rows[16]{};
    if (c.trace_vs_constants.size() >= sizeof(rows)) std::memcpy(rows, c.trace_vs_constants.data(), sizeof(rows));
    std::string constants;
    for (const float f : rows) constants += fmt::format("{}{}", constants.empty() ? "" : ",", JsonFloat(f));
    host::Record("native-d3d-probe-draw", fmt::format(
        "\"swap\":{},\"lr\":{},\"index_count\":{},\"base_vertex\":{},\"positions\":{},\"c0_c3\":[{}]", c.bind_swap,
        lr, index_count, base_vertex, positions, constants));
  }
  if (FrameTraceActive()) {
    std::string textures;
    for (const auto& t : request.textures)
      textures += fmt::format("{}{{\"reg\":{},\"key\":{},\"resolved\":{},\"cube\":{},\"format\":{},\"size\":[{},{}],"
                              "\"levels\":{}}}", textures.empty() ? "" : ",", t.reg, t.key, t.resolved, t.cube,
                              uint32_t(t.format), t.width, t.height, t.levels);
    // The first draw of each (VS, PS, first texture): the descriptor, stream
    // endian and raw format of each element, the raw elements of its first
    // three vertices, the nonzero constants (VS, PS) and its texture records.
    std::string diag;
    const std::string pair = fmt::format("{}|{}|{}", request.vs.key, request.ps.key,
                                         request.textures.empty() ? 0u : request.textures[0].key);
    if (!quad && frame_trace.pairs.size() < 400 && frame_trace.pairs.insert(pair).second) {
      std::string fetch, vertices, texture_records;
      const std::vector<VertexElement>& trace_elements = *request.elements;
      for (size_t i = 0; i < layout.fetch.size() && i < trace_elements.size(); ++i) {
        uint32_t descriptor = 0;
        std::memcpy(&descriptor, &request.shared_constants[320 + 4 * i], 4);
        fetch += fmt::format("{}[{},{},{}]", i ? "," : "", descriptor, stream_endian[layout.fetch[i].slot],
                             uint32_t(trace_elements[i].format));
      }
      // A hosted index or stream is read from guest memory, swapped as its upload was.
      auto guest_word = [&](uint32_t physical, uint32_t bytes, uint32_t endian, uint32_t& word) {
        std::vector<uint8_t> raw(bytes);
        word = 0;
        if (!ReadPhysical(base, physical, bytes, raw.data()) || !SwapEndian(raw, endian)) return false;
        std::memcpy(&word, raw.data(), bytes);
        return true;
      };
      for (uint32_t k = 0; k < std::min<uint32_t>(index_count, 3); ++k) {
        uint32_t v = 0;
        if (size_t(k + 1) * index_size <= request.indices.size())
          std::memcpy(&v, &request.indices[size_t(k) * index_size], index_size);
        else if (!indexed || !guest_word(t.index_physical + k * index_size, index_size, t.index_endian, v))
          break;
        std::string words;
        for (size_t i = 0; i < trace_elements.size(); ++i) {
          const auto& e = trace_elements[i];
          const auto& data = request.streams[e.slot].data;
          const int64_t at = (int64_t(v) + base_vertex) * request.streams[e.slot].stride + e.offset;
          const uint32_t n = e.format == VertexFormat::kR32G32B32A32Uint ? 4
                             : e.format == VertexFormat::kR32G32B32Uint ? 3
                             : e.format == VertexFormat::kR32G32Uint ? 2 : 1;
          std::string w;
          for (uint32_t j = 0; j < n; ++j) {
            uint32_t word = 0;
            if (at >= 0 && size_t(at) + 4 * j + 4 <= data.size()) std::memcpy(&word, &data[size_t(at) + 4 * j], 4);
            else if (at >= 0 && request.streams[e.slot].key && e.slot < stream_address.size())
              guest_word(stream_address[e.slot] + uint32_t(at) + 4 * j, 4, stream_endian[e.slot], word);
            w += fmt::format("{}{}", j ? "," : "", word);
          }
          words += fmt::format("{}[{}]", i ? "," : "", w);
        }
        vertices += fmt::format("{}{{\"index\":{},\"elements\":[{}]}}", k ? "," : "", v, words);
      }
      auto nonzero = [](const std::vector<uint8_t>& c) {
        std::string out;
        for (uint32_t r = 0; r < 256 && 16 * (r + 1) <= c.size(); ++r) {
          uint32_t w[4];
          std::memcpy(w, &c[16 * r], 16);
          if (!(w[0] | w[1] | w[2] | w[3])) continue;
          float f[4];
          std::memcpy(f, w, 16);
          out += fmt::format("{}[{},{},{},{},{}]", out.empty() ? "" : ",", r, JsonFloat(f[0]), JsonFloat(f[1]),
                             JsonFloat(f[2]), JsonFloat(f[3]));
        }
        return out;
      };
      for (const auto& t : request.textures) {
        texture_records += fmt::format("{}{{{}}}", texture_records.empty() ? "" : ",", TextureRecord(base, t.reg, t.reg >= 16));
        const bool known = std::any_of(frame_trace.sampled.begin(), frame_trace.sampled.end(),
                                       [&](const auto& s) { return s.first == t.key; });
        if (!t.resolved && !t.cube && !known && frame_trace.sampled.size() < 96)
          frame_trace.sampled.emplace_back(t.key, t.levels);
      }
      diag = fmt::format(",\"diag\":{{\"fetch\":[{}],\"elements\":[{}],\"streams\":[{}],\"vertices\":[{}],"
                         "\"vs_c\":[{}],\"ps_c\":[{}],\"texture_records\":[{}]}}", fetch, elements_record,
                         streams_text(), vertices, nonzero(c.trace_vs_constants), nonzero(c.trace_ps_constants),
                         texture_records);
    }
    TraceFrameOp("draw", fmt::format(
        "\"entry\":\"{}\",\"lr\":{},\"vs\":\"{}\",\"ps\":\"{}\",\"color_key\":{},\"depth_key\":{},\"depth_only\":{},"
        "\"depth\":[{},{},{}],\"stencil\":{},\"color_mask\":{},\"blend_control\":{},\"alpha_to_coverage\":{},"
        "\"cull\":[{},{}],\"viewport\":[{},{},{},{},{},{}],\"scissor\":[{},{},{},{}],\"topology\":\"{}\","
        "\"index_count\":{},\"base_vertex\":{},\"textures\":[{}]{}", name, lr, request.vs.key, request.ps.key,
        request.color_key,
        request.depth_key, request.depth_only, request.depth_enable, request.depth_write, request.depth_func,
        request.stencil_enable, request.color_mask, request.blend_control, request.alpha_to_coverage,
        request.cull_front, request.cull_back, JsonFloat(request.viewport[0]), JsonFloat(request.viewport[1]),
        JsonFloat(request.viewport[2]), JsonFloat(request.viewport[3]), JsonFloat(request.viewport[4]),
        JsonFloat(request.viewport[5]), request.scissor[0], request.scissor[1], request.scissor[2], request.scissor[3],
        TopologyName(request.topology), request.index_count, request.base_vertex, textures, diag));
  }
  // The constants the capture took (the whole blocks for the quad and
  // the evidence draws, the pending groups for the batched draws), moved.
  request.vs_constants = std::move(c.vs_constants);
  request.ps_constants = std::move(c.ps_constants);
  request.vs_constants_delta = std::move(c.vs_constants_delta);
  request.ps_constants_delta = std::move(c.ps_constants_delta);
  if (quad) {  // Kept for the take, with the vertices (PostInlineQuad).
    t.quad_record = fmt::format("{},\"elements\":[{}],\"streams\":[{}],{},\"ps_key\":\"{}\",\"textures\":[{}],"
                                "\"color_key\":{},\"depth_key\":{}", shaders_record.get(), elements_record, streams_text(),
                                state_record.get(), request.ps.key, textures_record, request.color_key,
                                request.depth_key);
    request.want_evidence = false;
    return t;
  }
  auto* dispatcher = host::ActiveDispatcher();
  profile::Timer engine_time(profile::kDrawEngine);
  // The draw is posted. Only the draws whose record needs the
  // engine evidence wait for it (evidence_draw, decided at the capture).
  const bool evidence_draw = c.evidence_draw;
  auto draw_failed = [&](const std::string& engine_evidence) {
    host::Record("native-d3d-draw-failed", fmt::format(
        "\"entry\":\"{}\",\"lr\":{},{},\"textures\":[{}],\"posted\":{},\"engine\":{}", name, lr, shaders_record.get(),
        textures_record, !evidence_draw, engine_evidence.empty() ? "null" : engine_evidence));
    sink.Stop(fmt::format("{}-own-draw-unproved", name), 88);
  };
  if (!dispatcher) draw_failed("");
  if (dispatcher->PostedFailure(t.evidence)) draw_failed(t.evidence);
  request.want_evidence = evidence_draw;
  // A batched draw takes its pipeline as BatchedPipelineTake says (the START
  // policy keeps the synchronous take; its keys carry the policy name).
  if (!evidence_draw && request.vs.key.find(':') == std::string::npos) request.pipeline_take = BatchedPipelineTake();
  if (evidence_draw) {
    uint64_t fence = 0;
    if (!dispatcher->Run([&request, &fence](Engine& engine, std::string& out) {
          return engine.DrawIndexed(request, fence, out); }, t.evidence))
      draw_failed(t.evidence);
    t.fence = fence;
  } else if (!dispatcher->PostDraw(std::move(request))) {
    // Moved into the batch: after this point the draw reads the shadows, not the request.
    dispatcher->PostedFailure(t.evidence);
    draw_failed(t.evidence);
  }
  engine_time.Stop();
  device_owner->NoteSubmission();
  t.n = c.n ? c.n : own_draws.fetch_add(1) + 1;
  // What the records of the title thread take (the first draws, the START census).
  t.max_index = max_index;
  t.stream0 = stream0;
  t.depth_control = depth_control;
  t.blend0 = s.blend0;
  t.color_control = color_control;
  t.mode = mode;
  t.vte = vte;
  t.color_mask_raw = s.color_mask_raw;
  if (want_records) {
    t.shaders_record = shaders_record.get();
    t.state_record = state_record.get();
    t.textures_record = std::move(textures_record);
    if (t.n <= 16) {
      t.index_record = index_record.get();
      t.streams_record = streams_text();
      // Stream 0 words as floats (null when not finite: packed colors or normals).
      const auto& s0 = request.streams[0];
      for (size_t i = 0; i < std::min<size_t>(s0.data.size() / 4, 24); ++i) {
        float f;
        std::memcpy(&f, &s0.data[4 * i], 4);
        t.vertex_record += fmt::format("{}{}", i ? "," : "", JsonFloat(f));
      }
    }
  }
  return t;
}
// The inputs of a traced quad as the engine gets them: the
// vertices as read at the take, the state, the textures, c0..c3 of the VS
// (g_mWorldViewProj), the nonzero PS constants and the nonzero shared words
// (c16, c17, the fetch descriptors c20..c23, the clip planes c24..c29).
std::string QuadTraceFields(const PendingDraw& d, const InlineBuild& build, const char* by) {
  const DrawRequest& r = build.request;
  std::string vertices;
  if (build.slot < r.streams.size()) {
    const auto& data = r.streams[build.slot].data;
    for (size_t i = 0; i + 4 <= data.size(); i += 4) {
      float f;
      std::memcpy(&f, &data[i], 4);
      vertices += fmt::format("{}{}", i ? "," : "", JsonFloat(f));
    }
  }
  auto registers = [](std::span<const uint8_t> block, uint32_t first, uint32_t count, bool nonzero) {
    if (block.size() != 4096) return std::string("\"not-whole\"");
    std::string out;
    for (uint32_t reg = first; reg < first + count; ++reg) {
      uint32_t w[4];
      float f[4];
      std::memcpy(w, &block[16 * reg], 16);
      std::memcpy(f, &block[16 * reg], 16);
      if (nonzero && !(w[0] | w[1] | w[2] | w[3])) continue;
      out += fmt::format("{}[{},{},{},{},{}]", out.empty() ? "" : ",", reg, JsonFloat(f[0]), JsonFloat(f[1]),
                         JsonFloat(f[2]), JsonFloat(f[3]));
    }
    return "[" + out + "]";
  };
  std::string shared;
  for (uint32_t at = 256; at < 480; at += 4) {
    uint32_t w = 0;
    std::memcpy(&w, &r.shared_constants[at], 4);
    if (w) shared += fmt::format("{}[{},{}]", shared.empty() ? "" : ",", at, w);
  }
  std::string textures;
  for (const auto& t : r.textures)
    textures += fmt::format("{}{{\"reg\":{},\"key\":{},\"resolved\":{},\"format\":{},\"size\":[{},{}],\"levels\":{},"
                            "\"mapping\":\"{}\",\"filter\":\"{}/{}/{}{}\",\"address\":[\"{}\",\"{}\"],\"version\":{}}}",
                            textures.empty() ? "" : ",", t.reg, t.key, t.resolved, uint32_t(t.format), t.width,
                            t.height, t.levels, MappingName(t.component_mapping), FilterName(t.min_filter),
                            FilterName(t.mag_filter), FilterName(t.mip_filter),
                            t.anisotropic ? fmt::format("+anisotropic{}", t.max_anisotropy) : std::string(),
                            AddressModeName(t.address_u), AddressModeName(t.address_v), t.version);
  std::string viewport;
  for (float f : r.viewport) viewport += fmt::format("{}{}", viewport.empty() ? "" : ",", JsonFloat(f));
  return fmt::format(
      "\"taken_by\":\"{}\",\"call\":{},\"build_thread\":{},\"take_thread\":{},\"lr\":{},\"vertices_at\":{},"
      "\"vertices\":[{}],\"vs\":\"{}\",\"ps\":\"{}\",\"color_key\":{},\"depth_key\":{},\"depth_only\":{},"
      "\"color_mask\":{},\"blend_control\":{},\"depth\":[{},{},{}],\"stencil\":{},\"cull\":[{},{}],\"topology\":\"{}\","
      "\"index_count\":{},\"base_vertex\":{},\"viewport\":[{}],\"scissor\":[{},{},{},{}],\"textures\":[{}],"
      "\"vs_c0_3\":{},\"ps_c\":{},\"shared_words\":[{}],\"versions\":[{},{}],\"async\":{}",
      by, d.call, d.thread, GuestThread(), d.lr, d.vertices, vertices, r.vs.key, r.ps.key, r.color_key, r.depth_key,
      r.depth_only, r.color_mask, r.blend_control, r.depth_enable, r.depth_write, r.depth_func, r.stencil_enable,
      r.cull_front, r.cull_back, TopologyName(r.topology), r.index_count, r.base_vertex, viewport, r.scissor[0],
      r.scissor[1],
      r.scissor[2], r.scissor[3], textures, registers(r.vs_constants, 0, 4, false),
      registers(r.ps_constants, 0, 256, true), shared, r.vs_constants_version, r.ps_constants_version,
      r.pipeline_take != DrawRequest::PipelineTake::kSync);
}
std::atomic<uint32_t> quad_snapshots{0};
// The quad with the vertices the title wrote after BeginVertices,
// posted on the engine owner before the next own operation of the thread.
// In a traced frame the quad runs with its engine evidence, its
// inputs go to a quad-take op, and its target is copied into own textures
// (0xFFFE0000 + k) right before and right after it, read back at the Swap.
std::string PostInlineQuad(PPCContext* ctx, uint8_t* base, const PendingDraw& d, std::shared_ptr<InlineBuild> build,
                           const char* by) {
  if (!build) return "\"gpu_draw\":false,\"reason\":\"no-request\"";
  if (!ctx) return "\"gpu_draw\":false,\"reason\":\"dropped-at-stop\"";
  // The own vertex area is a system-heap allocation, in the
  // virtual 4 KB heap; the title writes the vertices through that virtual
  // address, so they are read there (TranslateVirtual). Its physical alias
  // (GuestPhysicalAlias) is other memory in this runtime: it would read zeros,
  // a degenerate quad.
  auto* ts = rex::runtime::ThreadState::Get();
  auto* dispatcher = host::ActiveDispatcher();
  if (dispatcher && dispatcher->Deferred()) {
    // The 4 vertices of 20 bytes (the stride the commit forces) read here,
    // on the title; the stage fills the request its capture built and posts it.
    std::vector<uint8_t> bytes(InlineBuild::kBytes);
    if (!ts || !ReadVirtual(ts->memory(), d.vertices, bytes.data(), bytes.size()) ||
        !SwapEndian(bytes, InlineBuild::kEndian))
      StopInOrder(*ctx, "BeginVertices-quad-vertices-unreadable", 88);
    const uint64_t n = own_draws.fetch_add(1) + 1;
    if (!dispatcher->Enqueue(std::make_unique<QuadTakeTask>(std::move(build), std::move(bytes))))
      StopInOrder(*ctx, "BeginVertices-quad-translation-stage-closed", 88);
    return fmt::format("\"gpu_draw\":true,\"deferred\":true,\"own_draw\":{}", n);
  }
  auto& stream = build->request.streams[build->slot];
  if (stream.data.size() > InlineBuild::kBytes || !ts ||
      !ReadVirtual(ts->memory(), d.vertices, stream.data.data(), stream.data.size()) ||
      !SwapEndian(stream.data, InlineBuild::kEndian))
    StopInOrder(*ctx, "BeginVertices-quad-vertices-unreadable", 88);
  std::string evidence;
  const bool traced = FrameTraceActive();
  const std::string trace_fields = traced ? QuadTraceFields(d, *build, by) : std::string();
  const uint32_t target = build->request.color_key;
  const auto viewport = build->request.viewport;
  auto snapshot = [&](std::string& record) {
    ResolveRequest copy{};
    copy.source_key = Backing(target);
    copy.dest_key = 0xFFFE0000u | (quad_snapshots.fetch_add(1) & 0xFFFFu);
    copy.dest_width = uint32_t(viewport[2]);
    copy.dest_height = uint32_t(viewport[3]);
    copy.dest_format = 6;
    std::string copy_evidence;
    uint64_t copy_fence = 0;
    const bool copied = dispatcher && target && copy.dest_width && copy.dest_height &&
        dispatcher->Run([&copy, &copy_fence](Engine& engine, std::string& out) {
          return engine.ResolveSurface(copy, copy_fence, out); }, copy_evidence);
    if (copied) TraceFrameResolveDest(copy.dest_key);
    record = fmt::format("{{\"key\":{},\"copied\":{},\"engine\":{}}}", copy.dest_key, copied,
                         copy_evidence.empty() ? "null" : copy_evidence);
  };
  std::string before = "null", after = "null";
  if (traced) {
    snapshot(before);
    DrawRequest& request = build->request;
    request.want_evidence = true;
    uint64_t fence = 0;
    if (!dispatcher || dispatcher->PostedFailure(evidence) ||
        !dispatcher->Run([&request, &fence](Engine& engine, std::string& out) {
          return engine.DrawIndexed(request, fence, out); }, evidence)) {
      RecordPostedFailure("BeginVertices-quad");
      StopInOrder(*ctx, "BeginVertices-quad-own-draw-unproved", 88);
    }
  } else if (!dispatcher || dispatcher->PostedFailure(evidence) || !dispatcher->PostDraw(std::move(build->request))) {
    RecordPostedFailure("BeginVertices-quad");
    StopInOrder(*ctx, "BeginVertices-quad-own-draw-unproved", 88);
  }
  device_owner->NoteSubmission();
  const uint64_t n = own_draws.fetch_add(1) + 1;
  const uint64_t drawn = inline_quads_drawn.fetch_add(1) + 1;
  if (traced) {
    snapshot(after);
    TraceFrameOp("quad-take", fmt::format("{},\"engine\":{},\"before\":{},\"after\":{}", trace_fields,
                                          evidence.empty() ? "null" : evidence, before, after));
  }
  return fmt::format("\"gpu_draw\":true,\"own_draw\":{},\"quads_drawn\":{},{}", n, drawn, build->record);
}
// The take on the stage: a quad the census skipped has no own draw; a
// refused post asks the title to stop, as the inline take stops.
void QuadTakeTask::Run() {
  if (build_->census_skipped || build_->slot >= build_->request.streams.size()) return;
  auto& stream = build_->request.streams[build_->slot];
  stream.data = std::move(vertices_);
  auto* dispatcher = host::ActiveDispatcher();
  std::string evidence;
  if (!dispatcher || dispatcher->PostedFailure(evidence) || !dispatcher->PostDraw(std::move(build_->request))) {
    RecordPostedFailure("BeginVertices-quad");
    RequestStop("BeginVertices-quad-own-draw-unproved", 88);
    throw TranslationStopped{};
  }
  device_owner->NoteSubmission();
  inline_quads_drawn.fetch_add(1);
}
}  // namespace gpu

void BuildInlineQuad(const PPCContext& entry, uint8_t* base, uint32_t vertices, PPCContext& ctx) {
  auto build = std::make_shared<InlineBuild>();
  build->vertices = vertices;
  PPCContext request_ctx = entry;  // r3 = device; the guards stop through it.
  request_ctx.r1 = ctx.r1;
  // As the original during its commit: the stride of stream 0 (+12520, in
  // words) is the inline stride 5 (20 bytes) while the VS fetches are patched.
  const uint32_t dev = entry.r3.u32;
  const uint8_t stride = Get8(base, dev + 12520);
  Put8(base, dev + 12520, 5);
  OwnDraw(request_ctx, base, false, build);
  Put8(base, dev + 12520, stride);
  std::lock_guard lock(inline_draw.mutex);
  if (build->census_skipped) build.reset();  // The quad stays without own draw.
  inline_draw.build = std::move(build);
}


REX_HOOK_RAW(sub_82137E00) {
  NotePacingDraw(base, ctx.r3.u32, ctx.lr, ctx.r4.u32, ctx.r7.u32);  // Both modes.
  StateNoteDraw(ctx, base, 0);  // Both modes.
  if (!gpu::Active()) { __imp__sub_82137E00(ctx, base); return; }
  HookScope scope("DrawIndexedVertices");
  OwnDraw(ctx, base, true);
}
REX_HOOK_RAW(sub_82137A18) {
  StateNoteDraw(ctx, base, 1);  // Both modes.
  if (!gpu::Active()) { __imp__sub_82137A18(ctx, base); return; }
  HookScope scope("DrawVertices");
  OwnDraw(ctx, base, false);
}

// Interrupts that 8212F7E0(device, cursor, flags, routine, argument)
// inserts. Flags bit 1 (default 6) asks for the interrupt; bits 24..29 are the
// CPU mask (default 4, CPU 2). Its packets reach no consumer, so the request is
// kept here and the own Swap hands it to the own interrupt source once the frame
// batch has closed on the own queue. Only the swap routine 82138358 is covered.
namespace gpu {
std::mutex leaf_interrupt_mutex;
std::vector<LeafInterrupt> leaf_interrupts;
std::atomic<uint64_t> leaf_interrupt_count{0};
std::vector<LeafInterrupt> TakeLeafInterrupts() {
  std::lock_guard lock(leaf_interrupt_mutex);
  std::vector<LeafInterrupt> taken;
  taken.swap(leaf_interrupts);
  return taken;
}
}  // namespace gpu

namespace gpu {
bool AdmitDiscardLeaf(PPCContext& ctx) {
  if (!Active() || !discard_thread.load() || discard_thread.load() != GuestThread()) return false;
  const uint32_t cursor = ctx.r4.u32;
  if (cursor < inline_draw.discard || cursor >= inline_draw.discard + kDiscardLimit) return false;
  uint32_t flags = ctx.r5.u32;
  if (!(flags & 6)) flags |= 6;
  if (flags & 2) {
    uint32_t mask = (flags >> 24) & 0x3F;
    if (!mask) mask = 4;
    if (ctx.r6.u32 != 0x82138358) StopInOrder(ctx, fmt::format("interrupt-routine-uncovered-{:08X}", ctx.r6.u32), 86);
    std::lock_guard lock(leaf_interrupt_mutex);
    leaf_interrupts.push_back(LeafInterrupt{ctx.r6.u32, ctx.r7.u32, mask, uint32_t(ctx.lr)});
    leaf_interrupt_count.fetch_add(1);
  }
  discard_leaves.fetch_add(1);
  return true;
}
}  // namespace gpu

// 821384F0(device, flag), the "submission kick". It does not submit the ring
// or advance 10908: it writes GPU-side
// synchronization packets (WAIT_REG_MEM on the +10900 scratch, event writes by
// 8212F7E0) and CPU stores: 821440F8 (performance counters), 8213A670
// (predication state 12700..12708, 10932, 10936 and flags of 10940) and bit
// 0x10 of 10941. The own queue already executes every own operation in order,
// so the packets have no own equivalent; the original runs over the discard
// buffer to keep the CPU stores.
extern "C" void __imp__sub_821384F0(PPCContext& __restrict, uint8_t*);
namespace gpu {
std::atomic<uint64_t> kicks{0};
}  // namespace gpu
REX_HOOK_RAW(sub_821384F0) {
  if (!gpu::Active()) { __imp__sub_821384F0(ctx, base); return; }
  HookScope scope("Kick");
  if (!device_owner || !device_owner->live() || ctx.r3.u32 != device_owner->device()) StopInOrder(ctx, "Kick-device", 88);
  const uint32_t dev = ctx.r3.u32, flag = ctx.r4.u32, lr = ctx.lr;
  const uint32_t counter = Get32(base, dev + 10908), flags_before = Get8(base, dev + 10941);
  const uint32_t discarded = RunOverDiscard(ctx, base, dev, __imp__sub_821384F0, "Kick");
  if (Get32(base, dev + 10908) != counter) StopInOrder(ctx, "Kick-counter-changed", 88);
  if (kicks.fetch_add(1) < 8 && Diagnostics())
    gpu::host::Record("native-d3d-kick", fmt::format(
        "\"flag\":{},\"lr\":{},\"guest_thread\":{},\"discarded_bytes\":{},\"counter10908\":{},"
        "\"flags10941_before\":{},\"flags10941_after\":{},\"state12700\":[{},{},{}],\"own_draws\":{},"
        "\"discard_leaves\":{},\"packets_consumed\":false", flag, lr, GuestThread(), discarded, counter, flags_before,
        Get8(base, dev + 10941), Get32(base, dev + 12700), Get32(base, dev + 12704), Get32(base, dev + 12708),
        own_draws.load(), discard_leaves.load()));
}

// 821083B8, the scene item callback that 820FD5F8 reaches through the
// type table 0x82C259E0 (LR 820FD78C). Title code without arguments: it reads
// the current 3208-byte list, writes material constants into the device shadow
// (inlined SetVertexShaderConstantF/SetPixelShaderConstantF: VS +0x780 and PS
// +0x1780, one dirty bit per group of four in +0 and +8), sets the rest through
// library entries and calls the producer 821024A8 (LR 82108DF4). When bit
// 0x2000 of its material descriptor is set, the tail writes four ring dwords
// inline after the producer: EVENT_WRITE TILE_FLUSH (15) and WAIT_UNTIL =
// WAIT_3D_IDLECLEAN (0x5C8 = 0x20000). It then clears bit 0x04 of +10941, the
// pending tile flush that Resolve, the clear helper 82130988 and the depth
// commit 8213FAA8 set (8213FAA8 writes the same event alone). With the guard
// cursor the tail asks 8212F608 for space (LR 82108E24).
// The own queue runs the own operations in order with the engine's transitions,
// and the own surfaces have no tiles or Hi-Z, so neither the two packets nor the
// ring kick of 8212F608 have an own equivalent. The original runs and keeps its
// CPU effects; inside this scope the reserve at LR 82108E24 gets the own discard
// buffer, which no consumer reads, and the guard cursor is put back on return.
extern "C" void __imp__sub_821083B8(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_821083B8) {
  if (!gpu::Active()) { __imp__sub_821083B8(ctx, base); return; }
  if (!device_owner || !device_owner->live()) StopInOrder(ctx, "821083B8-without-live-device", 88);
  constexpr uint32_t kCursor = DeviceOwner::kCursor;
  const uint32_t dev = device_owner->device(), lr = ctx.lr, thread = GuestThread();
  if (Get32(base, dev + 48) != kCursor || Get32(base, dev + 56) != 0 || discard_thread.load() ||
      inline_draw.scope_thread.load() || scene_item_thread.load())
    StopInOrder(ctx, "821083B8-cursor-state", 88);
  const uint32_t flags_before = Get8(base, dev + 10941), end52 = Get32(base, dev + 52);
  const uint64_t flushes_before = scene_flushes.load(), draws_before = own_draws.load();
  scene_item_thread.store(thread);
  __imp__sub_821083B8(ctx, base);
  scene_item_thread.store(0);
  const uint64_t flushes = scene_flushes.load();
  const uint32_t cursor = Get32(base, dev + 48);
  if (flushes != flushes_before) {
    if (flushes != flushes_before + 1 || cursor != inline_draw.discard + 16)
      StopInOrder(ctx, "821083B8-tail-unproved", 88);
    Put32(base, dev + 48, kCursor);
  } else if (cursor != kCursor) {
    StopInOrder(ctx, "821083B8-cursor-changed", 88);
  }
  if (Get32(base, dev + 56) != 0 || Get32(base, dev + 52) != end52) StopInOrder(ctx, "821083B8-ring-state-changed", 88);
  const uint64_t n = ++scene_items;
  const bool flushed = flushes != flushes_before;
  if (Diagnostics() && (n <= 16 || (flushed && flushes <= 8)))
    gpu::host::Record("native-d3d-scene-item", fmt::format(
        "\"call\":{},\"lr\":{},\"guest_thread\":{},\"own_draws_inside\":{},\"own_draws\":{},\"tail_flush\":{},"
        "\"tail_bytes\":{},\"flags10941_before\":{},\"flags10941_after\":{},\"scene_flushes\":{},"
        "\"cursor_restored\":{},\"packets_consumed\":false", n, lr, thread, own_draws.load() - draws_before,
        own_draws.load(), flushed, flushed ? cursor - inline_draw.discard : 0, flags_before,
        Get8(base, dev + 10941), flushes, kCursor));
}

namespace gpu {
bool AdmitSceneFlushReserve(PPCContext& ctx, uint8_t* base) {
  const uint32_t thread = GuestThread();
  if (!Active() || ctx.lr != 0x82108E24 || !thread || scene_item_thread.load() != thread) return false;
  const uint32_t dev = ctx.r3.u32;
  if (!device_owner || !device_owner->live() || dev != device_owner->device() ||
      Get32(base, dev + 48) != DeviceOwner::kCursor || Get32(base, dev + 56) != 0 ||
      discard_thread.load() || !EnsureInlineAreas())
    return false;
  ctx.r3.u64 = inline_draw.discard;  // The tail writes 16 bytes after it and stores their end in +48.
  scene_flushes.fetch_add(1);
  return true;
}
}  // namespace gpu
