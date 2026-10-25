#pragma once

// State translation of the D3D boundary (state.cpp): the shadows of the
// constants and of the render state, refreshed by the dirty masks of the device.
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include "gpu/backend/engine.h"

namespace gpu {
// Constants and render state follow the dirty
// masks of the device. The title's own D3D keeps
// 12 words at device+0: +0 (64 bits) the VS float constants, bit 63 - i
// marking constants c4i..c4i+3 (64 bytes at 0x780 + 64 i); +8 the PS ones at
// 0x1780; +16 the flags and register blocks 0x2000, 0x2100, 0x2180 and 0x2200;
// +24 the fetch constants and block 0x2280; +32 blocks 0x2300 and 0x2380, the
// booleans and 0x4900; +40 the target flags. Every writer, inline ones too,
// ORs its bits, and the commit inside the three draws (82137E00,
// 82137A18, 821375A0: 821403E0, 82140040, 82140280, 82140100) sends the dirty
// runs and clears the words. The constant sub-commit 821403E0 has no other
// caller (IDA), so the draw reads the words before its own commit
// (dirty_before) and refreshes two shadows of the constants from them; a quad
// (its commit ran inside BeginVertices) and the START variation refresh
// everything. The register-block sub-commit 82140040 is also called by the
// clear helper 82130988, the Resolve 821398C8 and 821461D0, which consume
// +16..+39 and re-dirty only the blocks they touched, so the translated
// state follows a value snapshot of its registers instead, compared by value.
struct ConstantShadow {
  std::array<uint8_t, 4096> bytes{};
  bool valid = false;
  uint64_t version = 0;  // Carried by the request: the engine reuses the CBV of the batch when unchanged.
  // The groups (64 bytes, bit 63 - group as the title's mask) refreshed
  // since the last request that carried them; all ones until the engine has
  // seen the whole block.
  uint64_t pending = ~0ull;
};
extern ConstantShadow vs_shadow, ps_shadow;  // Render thread only.
extern std::atomic<uint64_t> state_refreshes, state_reuses;
void RefreshConstants(uint8_t* base, uint32_t at, uint32_t mask_hi, uint32_t mask_lo, bool full, ConstantShadow& s);
void TakeConstants(ConstantShadow& s, bool whole, ConstantBytes& block, ConstantBytes& delta);
// A draw the translation skipped gives back what its take carried: the
// groups pending before it (pending_before) and the counts of the take (a
// whole block, or delta_bytes of delta records).
void ReturnConstants(ConstantShadow& s, uint64_t pending_before, bool whole, size_t delta_bytes);
// The constants a draw the translation skipped carried (a whole block or
// delta records), kept by the translator and joined to the next request:
// JoinConstants puts the carry in front of block/delta (a whole block wins
// over a carried delta; a carried whole block takes the delta records) and
// empties the carry.
struct ConstantCarry {
  ConstantBytes block, delta;
  bool any() const { return !block.empty() || !delta.empty(); }
};
void JoinConstants(ConstantCarry& carry, ConstantBytes& block, ConstantBytes& delta);
// After an out-of-band draw replaced the engine's blocks (the isolated START
// run), the next request carries the whole blocks again.
void ForceFullConstants();
extern std::atomic<uint32_t> clip_plane_records;  // The first draws with user clip planes.
// The translated render state of the request, kept between draws.
struct StateKey {
  std::array<uint32_t, 5> targets{};  // +12432..+12448: RT0..RT3 and depth headers.
  bool depth_only = false;
  Topology topology = Topology::kTriangleList;
  bool operator==(const StateKey& o) const {
    return targets == o.targets && depth_only == o.depth_only && topology == o.topology;
  }
};
// The registers the translation reads: the blocks 0x2200 (12), 0x2100 (21),
// 0x2000 (19), 0x2300 (8) and 0x2380 (4), and the format and flags of the
// color header (the covered surface formats).
constexpr unsigned kStateRegisterCount = 12 + 21 + 19 + 8 + 4 + 2;
constexpr unsigned kColorFormatRegister = 64, kColorInfoRegister = 65;  // Color header +0x28 and +0x1C.
using StateRegisters = std::array<uint32_t, kStateRegisterCount>;
void ReadStateRegisters(uint8_t* base, uint32_t dev, StateRegisters& out);  // Title thread (the capture).
// The register n of the copy: reg(n) = device + block + 4 * (n - base register).
inline uint32_t StateRegister(const StateRegisters& r, uint32_t n) {
  if (n >= 0x2200 && n < 0x220C) return r[n - 0x2200];
  if (n >= 0x2100 && n < 0x2115) return r[12 + (n - 0x2100)];
  if (n >= 0x2000 && n < 0x2013) return r[33 + (n - 0x2000)];
  if (n >= 0x2300 && n < 0x2308) return r[52 + (n - 0x2300)];  // PA_SU_VTX_CNTL.
  if (n >= 0x2380 && n < 0x2384) return r[60 + (n - 0x2380)];  // PA_SU_POLY_OFFSET_*.
  return 0u;
}
struct StateShadow {
  bool valid = false;
  StateKey key;
  StateRegisters registers{};
  // Raw registers, for the records and the START inputs.
  uint32_t depth_control = 0, color_control = 0, mode = 0, vte = 0, blend0 = 0, color_mask_raw = 0, refmask = 0,
           refmask_bf = 0;
  float alpha_ref = 0;
  uint32_t ps_spec = 0;
  uint32_t clip_planes = 0;  // PA_CL_CLIP_CNTL.ucp_ena of the state (planes read every draw).
  const char* uncovered_field = nullptr;  // Re-raised every draw while the state holds.
  // Translated request fields.
  uint32_t blend_control = 0x00010001, color_mask = 0xF;
  bool alpha_to_coverage = false;
  std::array<float, 4> blend_factor{};
  bool depth_enable = false, depth_write = false;
  uint32_t depth_func = 7;
  bool stencil_enable = false;
  uint32_t stencil_front = 7, stencil_back = 7, stencil_ref = 0, stencil_read_mask = 0xFF, stencil_write_mask = 0xFF;
  bool cull_front = false, cull_back = false, front_ccw = true;
  int32_t depth_bias = 0;
  float depth_bias_slope = 0;
  std::array<float, 6> viewport{};
  std::array<int32_t, 4> scissor{};
  float half[2] = {0, 0};
};
extern StateShadow state_shadow;  // Render thread only.
void CaptureState(const DrawRequest& r, StateShadow& s);
void ApplyState(const StateShadow& s, DrawRequest& r);
std::string StateShadowSummary();
}  // namespace gpu
