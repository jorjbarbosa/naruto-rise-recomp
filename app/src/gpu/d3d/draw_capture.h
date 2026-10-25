#pragma once

// The capture of an own draw (draw.cpp): what the title's render thread copies
// at D3DDevice_DrawIndexedVertices, DrawVertices and the BeginVertices quad, so
// that the translation into the engine request reads no device word and no
// guest context. The hook is split into CaptureDraw, on the title thread, and
// TranslateDraw, on the translation stage (or inline on the same thread).
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "gpu/backend/engine.h"
#include "gpu/buffers/vertex_layout.h"
#include "gpu/d3d/state.h"
#include "gpu/diagnostics/start_evidence.h"
#include "gpu/shaders/shaders.h"
#include "gpu/textures/textures.h"

namespace gpu {
struct DrawCapture {
  // The call.
  const char* name = "";  // DrawIndexedVertices, DrawVertices or BeginVertices-quad.
  uint8_t* base = nullptr;  // Guest memory base: vertices, indices and textures are read at GPU time.
  bool indexed = false, quad = false;
  uint32_t primitive = 0, lr = 0, thread = 0;
  int32_t base_vertex = 0;
  uint32_t start_index = 0, index_count = 0, start_vertex = 0;
  uint32_t quad_vertices = 0;  // The own vertex area of the quad (guest virtual).
  bool measuring = false, want_records = false, evidence_draw = false;
  uint64_t bind_swap = 0;  // The Swap the draw belongs to (buffer probation).
  uint64_t n = 0;  // The own draw number, given at the capture when deferred; 0: given at the post (inline).
  uint32_t discarded = 0;  // Bytes of the original commit over the discard buffer.
  // The bound shaders: the registry entries or the per-draw read of an
  // object without entry, identified; the vertex layout of the pair.
  uint32_t vs_object = 0, ps_object = 0;
  std::shared_ptr<ShaderEntry> vs_entry, ps_entry;
  BoundShader vs_local, ps_local;
  const BoundShader& vs() const { return vs_entry ? vs_entry->bound : vs_local; }
  const BoundShader& ps() const { return ps_entry ? ps_entry->bound : ps_local; }
  bool depth_only = false;
  CachedShader* vs_cached = nullptr;
  CachedShader* ps_cached = nullptr;
  uint32_t vs_candidates = 0, ps_candidates = 0, vs_diff = 0, ps_diff = 0;
  std::shared_ptr<const VertexLayout> layout;
  // The sampler table of the PS: the entry's (read once) or this draw's.
  bool samplers_ok = false;
  std::vector<ShaderSampler> samplers_local;
  const std::vector<ShaderSampler>& ps_samplers() const { return ps_entry ? ps_entry->samplers : samplers_local; }
  // Fetch constants: the slots the commit changed (the translation keeps the
  // copy); TextureVerify: the slots whose live words differ from the copy.
  FetchDelta fetch;
  uint32_t fetch_missed = 0;
  // Registers after the original commit: the blocks (ReadStateRegisters), the
  // bound headers +12432..+12448 with the EDRAM base of each, the index buffer
  // +12428 with its first word and its base +0x18, the enabled user clip planes.
  StateRegisters registers{};
  std::array<uint32_t, 5> targets{}, target_edram{};
  // The own surfaces the draw renders into: the bound headers, or their tiling
  // backings while the title is inside BeginTiling..EndTiling (Backing, a
  // title-thread map that EndTiling clears before the stage translates the
  // draws of the pass).
  uint32_t color_backing = 0, depth_backing = 0;
  uint32_t ib = 0, ib_common = 0, ib_base = 0;
  std::array<float, 24> clip_planes{};
  // Constants: what the request carries, taken after the guards of the
  // capture, the versions of the shadows and what was pending before the take
  // (given back when the translation skips the draw).
  ConstantBytes vs_constants, ps_constants, vs_constants_delta, ps_constants_delta;
  uint64_t vs_constants_version = 0, ps_constants_version = 0;
  uint64_t vs_pending_before = 0, ps_pending_before = 0;
  // The whole shadows in a traced frame; the first four vertex rows for the
  // draw probe (REX_NATIVE_PROBE_TEXTURE).
  std::vector<uint8_t> trace_vs_constants, trace_ps_constants;
  bool vs_taken_whole = false, ps_taken_whole = false;  // What the take counted (ReturnConstants).
  size_t vs_taken_delta = 0, ps_taken_delta = 0;
  // START evidence, read on the title thread; allocated only on the routes
  // that read it (records, variation, the lot's candidate), since a
  // production draw would pay the zeroing of its ~6 KB.
  std::array<uint32_t, 12> dirty_before{};
  std::array<uint32_t, 3> cursor_before{};
  std::unique_ptr<StartMetadata> before_metadata;
  std::unique_ptr<StartLiveInputs> live;
};
// Where a guard of the translation goes after its record: inline, it stops
// in order with the context saved before the original commit; deferred, it
// requests the stop and leaves the translation. It never returns.
class GuardSink {
 public:
  virtual ~GuardSink() = default;
  [[noreturn]] virtual void Stop(const std::string& reason, int code) = 0;
};
// The translation of a capture: the request as posted (moved into the batch,
// or whole after an evidence draw), the own draw number, the fence and the
// engine evidence, and what the records of the title thread print.
struct DrawTranslation {
  DrawRequest request;
  uint64_t n = 0, fence = 0;
  std::string evidence, policy = "null";
  uint32_t max_index = 0, ib_descriptor = 0, index_physical = 0, index_endian = 0;
  std::array<uint32_t, 3> stream0{};
  uint32_t depth_control = 0, blend0 = 0, color_control = 0, mode = 0, vte = 0, color_mask_raw = 0;
  std::string shaders_record, state_record, textures_record, index_record, streams_record, vertex_record;
  // The quad: the slot of its fetch 95 and the record of its build.
  uint32_t quad_slot = UINT32_MAX;
  std::string quad_record;
};
// Translates the capture and posts the draw (a quad is kept in the result for
// its take). Reads the capture (and moves its constant bytes into the request),
// the translation's own state (texture and buffer bindings, fetch copy, state
// shadow) and guest memory at GPU time; no device word, no guest context.
DrawTranslation TranslateDraw(DrawCapture& capture, GuardSink& sink);
}  // namespace gpu
