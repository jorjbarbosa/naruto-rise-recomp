#pragma once

// Host timers of the own frame. Each slot
// sums nanoseconds and calls over a frame on whatever thread runs the measured
// work (render thread, engine owner or another guest thread); the frame layer
// takes and resets them at each own Swap of the measurement mode
// (REX_NATIVE_MEASURE_SWAPS). steady_clock is QueryPerformanceCounter here.
#include <array>
#include <atomic>
#include <chrono>
#include "gpu/diagnostics/diagnostics.h"
#include <cstdint>
#include <cstdlib>

namespace gpu::profile {
enum Slot : unsigned {
  kDispatchRoundTrip,    // Dispatcher::Run, caller side: queue, owner and result.
  kEngineOperation,      // The operation itself, on the engine owner.
  kEngineDraw,           // Engine::DrawIndexed.
  kEnginePipeline,       // CreateGraphicsPipelineState inside DrawIndexed, or the take (and wait) of a precompiled one.
  kEngineTexture,        // Sampled texture creation inside DrawIndexed, and UploadSampled.
  kEngineBuffer,         // UploadBuffer (vertex and index buffers kept by the engine).
  kEngineUpload,         // Upload buffer creation, map and copies inside DrawIndexed.
  kEngineRecord,         // Allocator, command list and recording inside DrawIndexed.
  kEngineSubmit,         // ExecuteCommandLists and Signal inside DrawIndexed.
  kEngineClear,          // Engine::ClearSurfaces.
  kEngineResolve,        // Engine::ResolveSurface.
  kEngineWaitIdle,       // Engine::WaitIdle.
  kDraw,                 // OwnDraw, entry to return.
  kDrawOriginal,         // RunOverDiscard of the draw commit.
  kDrawShaders,          // Bound shader read, fetch patch and identification.
  kDrawTextures,         // BindTexture of the PS samplers (a read and upload when an event changed one).
  kDrawTextureHash,      // The content hash of the texture verification only (TextureVerify).
  kDrawShaderLoad,       // LoadDxil/LoadShader (the spec constant variant from the package included).
  kDrawStreams,          // Index and vertex reads, endian swap and expansion.
  kDrawEngine,           // Dispatcher::Run of the draw (round trip).
  kDrawEvidence,         // NoteStartDraw and draw records.
  kSwap,                 // Own Swap hook.
  kSwapWaitIdle,         // CloseFrameBatch: the WaitIdle round trip.
  kClear,                // Own clear hooks (ClearF, BeginTiling, Resolve clears).
  kResolve,              // Own resolve hooks.
  kFenceWait,            // BlockOnFence hook.
  kSwapPace,             // The limiter at the end of the own Swap.
  // The own work of the hooks outside the draw (time of a frame that no
  // slot above measured), and the creation of a sampled copy in the engine.
  kHookCreate,           // D210 CreateTexture and D330 CreateSurface, after the original.
  kHookDestroy,          // D750: the release of the own resources of a header.
  kHookLock,             // D950 Lock (with the original), and the C7E8 texture lock after its original.
  kHookUnlock,           // DBE0, the last Unlock: write ranges and the texture upload (read, distill, post).
  kHookShader,           // 820F5150/820F5348: the registration of a shader object.
  kEngineTextureCreate,  // CreateCommittedResource of a sampled copy inside UploadSampled.
  kTranslate,            // TranslateDraw, on the translator when deferred, inside kDraw otherwise.
  kCount
};
inline constexpr const char* kSlotNames[kCount] = {
    "dispatch_round_trip", "engine_operation", "engine_draw", "engine_pipeline", "engine_texture", "engine_buffer",
    "engine_upload", "engine_record", "engine_submit", "engine_clear", "engine_resolve", "engine_wait_idle",
    "draw", "draw_original", "draw_shaders", "draw_textures", "draw_texture_hash", "draw_shader_load",
    "draw_streams", "draw_engine", "draw_evidence", "swap", "swap_wait_idle", "clear", "resolve", "fence_wait",
    "swap_pace", "hook_create", "hook_destroy", "hook_lock", "hook_unlock", "hook_shader", "engine_texture_create",
    "translate"};
enum Counter : unsigned {
  kHashedBytes, kUploadBytes, kTextureUploads, kPipelinesCreated, kShaderVariants, kSubmissions,
  // Buffer uploads of the engine, their bytes, and the stream and index bytes still carried by draws.
  kBufferUploads, kBufferUploadBytes, kBufferInlineBytes,
  // Draw batches posted to the owner, and the constant bytes the draws carried (whole blocks and deltas).
  kDrawBatches, kConstantBytes,
  // Texture entries registered, bases forgotten
  // by the D750 and bases registered again after a D750, over the frame; and,
  // sampled at the Swap that closes it, the Swap number, the texture bases the
  // engine keeps a copy of, the live headers of the resource registry and the
  // hosted buffers.
  kTexturesRegistered, kTexturesForgotten, kTexturesRecreated, kSwapNumber, kTextureBases, kHeadersLive,
  kBuffersHosted,
  // Pipelines a draw took from the precompiled set (ready, or
  // waited for while a pool thread created it), and pool creations.
  kPipelinesAdopted, kPipelinesWaited, kPipelinesPrecompiled,
  // Write-watch notifications of the SDK and their pages, and the draws
  // with user clip planes (PA_CL_CLIP_CNTL.ucp_ena).
  kWatchNotifications, kWatchPages, kClipPlaneDraws,
  // Pipelines posted to the asynchronous threads by draws, and the draws
  // recorded without themselves while their pipeline was being created.
  kPipelinesAsync, kPipelineSkippedDraws,
  // The translation stage, per frame: the enqueues that waited
  // for room and the deepest the queue got (sampled at the Swap).
  kStageWaits, kStageDepth,
  // Absent pipelines of batched draws created in place because their triple
  // was warm, and those slower than the warm limit (which turns it off in the
  // skip take; a driver that does not keep the shaders otherwise).
  kPipelinesWarm, kPipelinesWarmSlow,
  // Pipelines the owner posted before drawing their batch, and the longest
  // wait of a draw for a pipeline being created in the frame (Max, microseconds).
  kPipelinesPrefetched, kPipelineWaitUsMax,
  // The constant refreshes of the capture (VS and PS) and the 64-byte groups
  // they read, and those that found their bytes already in the shadow: a
  // refresh with every group unchanged still raises the version, so the owner
  // uploads and binds a new CBV.
  kConstantRefreshes, kConstantRefreshesUnchanged, kConstantGroupsRead, kConstantGroupsUnchanged,
  // Owner draws recorded right after a draw of the same batch with the same
  // pipeline, constants, textures, targets and fixed state (concatenating the
  // geometry would fuse them); of those, the ones with the same vertex
  // streams, and those whose indices also continue the previous draw's in the
  // same buffer (fusable as they are).
  kDrawsSameState, kDrawsSameStreams, kDrawsFusable, kCounterCount
};
inline constexpr const char* kCounterNames[kCounterCount] = {
    "hashed_bytes", "upload_bytes", "texture_uploads", "pipelines_created", "shader_variants", "submissions",
    "buffer_uploads", "buffer_upload_bytes", "buffer_inline_bytes", "draw_batches", "constant_bytes",
    "textures_registered", "textures_forgotten", "textures_recreated", "swap_number", "texture_bases",
    "headers_live", "buffers_hosted", "pipelines_adopted", "pipelines_waited", "pipelines_precompiled",
    "watch_notifications", "watch_pages", "clip_plane_draws", "pipelines_async", "pipeline_skipped_draws",
    "stage_waits", "stage_depth", "pipelines_warm", "pipelines_warm_slow", "pipelines_prefetched",
    "pipeline_wait_us_max", "constant_refreshes", "constant_refreshes_unchanged", "constant_groups_read",
    "constant_groups_unchanged", "draws_same_state", "draws_same_streams", "draws_fusable"};

struct Accumulator {
  std::atomic<uint64_t> ns{0}, calls{0};
};
inline std::array<Accumulator, kCount> slots;
inline std::array<std::atomic<uint64_t>, kCounterCount> counters{};

// The measurement mode (REX_NATIVE_MEASURE_SWAPS set) keeps the production
// journal: a record written for every frame is kept only for its first
// kQuietRecords calls.
constexpr uint64_t kQuietRecords = 16;
inline bool QuietRecords() {
  static const bool value = [] {
    const char* text = std::getenv("REX_NATIVE_MEASURE_SWAPS");
    return text && *text;
  }();
  return value;
}

inline uint64_t Now() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline void Add(Slot slot, uint64_t ns) {
  slots[slot].ns.fetch_add(ns, std::memory_order_relaxed);
  slots[slot].calls.fetch_add(1, std::memory_order_relaxed);
}
// The profile runs with the diagnostic instruments (gpu::Diagnostics);
// off, a timer or a count costs one test.
inline void Count(Counter counter, uint64_t value) {
  if (gpu::Diagnostics()) counters[counter].fetch_add(value, std::memory_order_relaxed);
}
// A counter that keeps the frame's largest value instead of the sum.
inline void Max(Counter counter, uint64_t value) {
  if (!gpu::Diagnostics()) return;
  uint64_t seen = counters[counter].load(std::memory_order_relaxed);
  while (seen < value && !counters[counter].compare_exchange_weak(seen, value, std::memory_order_relaxed)) {}
}

class Timer {
 public:
  explicit Timer(Slot slot) : slot_(gpu::Diagnostics() ? slot : kCount), start_(slot_ == kCount ? 0 : Now()) {}
  ~Timer() { Stop(); }
  void Stop() {
    if (slot_ == kCount) return;
    Add(slot_, Now() - start_);
    slot_ = kCount;
  }
  Timer(const Timer&) = delete;
  Timer& operator=(const Timer&) = delete;
 private:
  Slot slot_;
  uint64_t start_;
};
}  // namespace gpu::profile
