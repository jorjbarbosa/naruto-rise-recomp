// State translation of the D3D boundary (state.h).
#include "gpu/d3d/state.h"
#include "gpu/common/guest_memory.h"
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
ConstantShadow vs_shadow, ps_shadow;  // Render thread only.
std::atomic<uint64_t> constant_refreshes{0}, constant_full_refreshes{0}, state_refreshes{0}, state_reuses{0};
std::atomic<uint64_t> constant_whole_requests{0}, constant_delta_requests{0}, constant_delta_bytes{0};
void RefreshConstants(uint8_t* base, uint32_t at, uint32_t mask_hi, uint32_t mask_lo, bool full, ConstantShadow& s) {
  if (!s.valid) full = true;
  if (!full && !mask_hi && !mask_lo) return;
  const uint64_t mask = uint64_t(mask_hi) << 32 | mask_lo;
  for (unsigned group = 0; group < 64; ++group) {
    if (!full && !((mask >> (63 - group)) & 1)) continue;
    for (unsigned i = 0; i < 16; ++i) {
      const uint32_t v = Get32(base, at + 64 * group + 4 * i);
      std::memcpy(&s.bytes[64 * group + 4 * i], &v, 4);
    }
  }
  s.pending |= full ? ~0ull : mask;
  s.valid = true;
  ++s.version;
  ++constant_refreshes;
  if (full) ++constant_full_refreshes;
}
// What a request carries of a shadow: the whole
// block (the evidence draws, the quad, the START lot, or until the engine
// saw everything) or the pending groups as delta records (uint16 first
// register, uint16 count, the registers), merged into runs. Taken at the
// end of the capture, after its guards; a draw the translation skips gives
// them back (ReturnConstants), so its groups stay pending.
void TakeConstants(ConstantShadow& s, bool whole, std::vector<uint8_t>& block, std::vector<uint8_t>& delta) {
  block.clear();
  delta.clear();
  if (whole || s.pending == ~0ull) {
    block.assign(s.bytes.begin(), s.bytes.end());
    s.pending = 0;
    ++constant_whole_requests;
    return;
  }
  unsigned g = 0;
  while (g < 64) {
    if (!((s.pending >> (63 - g)) & 1)) { ++g; continue; }
    unsigned e = g;
    while (e < 64 && ((s.pending >> (63 - e)) & 1)) ++e;
    const uint16_t first = uint16_t(4 * g), count = uint16_t(4 * (e - g));
    const size_t at = delta.size();
    delta.resize(at + 4 + size_t(count) * 16);
    std::memcpy(&delta[at], &first, 2);
    std::memcpy(&delta[at + 2], &count, 2);
    std::memcpy(&delta[at + 4], &s.bytes[size_t(first) * 16], size_t(count) * 16);
    g = e;
  }
  s.pending = 0;
  if (!delta.empty()) { ++constant_delta_requests; constant_delta_bytes += delta.size(); }
}
// A draw the translation skipped gives back what its take carried; the
// bytes did not move in between (the title refreshes before the next take).
void ReturnConstants(ConstantShadow& s, uint64_t pending_before, bool whole, size_t delta_bytes) {
  s.pending = pending_before;
  if (whole) {
    --constant_whole_requests;
  } else if (delta_bytes) {
    --constant_delta_requests;
    constant_delta_bytes -= delta_bytes;
  }
}
// The carry of a skipped draw in front of the next request's constants.
void JoinConstants(ConstantCarry& carry, std::vector<uint8_t>& block, std::vector<uint8_t>& delta) {
  if (!carry.any()) return;
  if (block.size() == 4096) {
    // The whole block of this draw replaces everything carried.
  } else if (carry.block.size() == 4096) {
    // The carried whole block takes the delta records of this draw (as the
    // engine applies them, in order), and the request carries the block.
    size_t at = 0;
    while (at + 4 <= delta.size()) {
      uint16_t first = 0, count = 0;
      std::memcpy(&first, &delta[at], 2);
      std::memcpy(&count, &delta[at + 2], 2);
      const size_t bytes = size_t(count) * 16;
      if (!count || size_t(first) + count > 256 || at + 4 + bytes > delta.size()) break;
      std::memcpy(&carry.block[size_t(first) * 16], &delta[at + 4], bytes);
      at += 4 + bytes;
    }
    block = std::move(carry.block);
    delta.clear();
  } else if (!carry.delta.empty()) {
    carry.delta.insert(carry.delta.end(), delta.begin(), delta.end());
    delta = std::move(carry.delta);
  }
  carry.block.clear();
  carry.delta.clear();
}
// After an out-of-band draw replaced the engine's blocks (the isolated START
// run), the next request carries the whole blocks again.
void ForceFullConstants() {
  vs_shadow.pending = ~0ull;
  ps_shadow.pending = ~0ull;
}
std::atomic<uint32_t> clip_plane_records{0};  // The first draws with user clip planes.
void ReadStateRegisters(uint8_t* base, uint32_t dev, StateRegisters& out) {
  unsigned n = 0;
  for (unsigned i = 0; i < 12; ++i) out[n++] = Get32(base, dev + 10548 + 4 * i);
  for (unsigned i = 0; i < 21; ++i) out[n++] = Get32(base, dev + 10444 + 4 * i);
  for (unsigned i = 0; i < 19; ++i) out[n++] = Get32(base, dev + 10368 + 4 * i);
  for (unsigned i = 0; i < 8; ++i) out[n++] = Get32(base, dev + 10680 + 4 * i);
  for (unsigned i = 0; i < 4; ++i) out[n++] = Get32(base, dev + 10832 + 4 * i);
  const uint32_t color_header = Get32(base, dev + 12432);
  out[n++] = color_header ? Get32(base, color_header + 0x28) : 0;
  out[n++] = color_header ? Get32(base, color_header + 0x1C) : 0;
}
StateShadow state_shadow;
void CaptureState(const DrawRequest& r, StateShadow& s) {
  s.blend_control = r.blend_control; s.color_mask = r.color_mask; s.alpha_to_coverage = r.alpha_to_coverage;
  s.blend_factor = r.blend_factor; s.depth_enable = r.depth_enable; s.depth_write = r.depth_write;
  s.depth_func = r.depth_func; s.stencil_enable = r.stencil_enable; s.stencil_front = r.stencil_front;
  s.stencil_back = r.stencil_back; s.stencil_ref = r.stencil_ref; s.stencil_read_mask = r.stencil_read_mask;
  s.stencil_write_mask = r.stencil_write_mask; s.cull_front = r.cull_front; s.cull_back = r.cull_back;
  s.front_ccw = r.front_ccw; s.depth_bias = r.depth_bias; s.depth_bias_slope = r.depth_bias_slope;
  s.viewport = r.viewport; s.scissor = r.scissor;
}
void ApplyState(const StateShadow& s, DrawRequest& r) {
  r.blend_control = s.blend_control; r.color_mask = s.color_mask; r.alpha_to_coverage = s.alpha_to_coverage;
  r.blend_factor = s.blend_factor; r.depth_enable = s.depth_enable; r.depth_write = s.depth_write;
  r.depth_func = s.depth_func; r.stencil_enable = s.stencil_enable; r.stencil_front = s.stencil_front;
  r.stencil_back = s.stencil_back; r.stencil_ref = s.stencil_ref; r.stencil_read_mask = s.stencil_read_mask;
  r.stencil_write_mask = s.stencil_write_mask; r.cull_front = s.cull_front; r.cull_back = s.cull_back;
  r.front_ccw = s.front_ccw; r.depth_bias = s.depth_bias; r.depth_bias_slope = s.depth_bias_slope;
  r.viewport = s.viewport; r.scissor = s.scissor;
}
std::string StateShadowSummary() {
  return fmt::format("\"constant_refreshes\":{},\"constant_full_refreshes\":{},\"constant_versions\":[{},{}],"
                     "\"state_refreshes\":{},\"state_reuses\":{},\"constant_whole_requests\":{},"
                     "\"constant_delta_requests\":{},\"constant_delta_bytes\":{}", constant_refreshes.load(),
                     constant_full_refreshes.load(), vs_shadow.version, ps_shadow.version, state_refreshes.load(),
                     state_reuses.load(), constant_whole_requests.load(), constant_delta_requests.load(),
                     constant_delta_bytes.load());
}
}  // namespace gpu
