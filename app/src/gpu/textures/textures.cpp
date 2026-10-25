// Textures of the D3D boundary (textures.h).
// Sampled texture of a draw from its fetch constant. Covered: the formats of
// textures/formats.cpp (the ones the game uses) without sign or exponent, at
// the places of the SDK's layout (textures/layout.cpp). A resolve destination
// is sampled from its own texture. Everything else is a guard.
//
// A texture is translated once and the draw only binds it. The title's
// SetTexture is inline: it writes the fetch constant at +0x480 + 24 * slot and
// ORs bit 31 - slot of the word at +28, the low half of +24. Only the three
// draw commits consume those bits (82140280 takes them shifted by 32, slot =
// cntlzd); the clear 82130988, the Resolve 821398C8 and 821461D0 clear only
// bits 34..54 of +24 (block 0x2280) and re-dirty the fetch slots they write.
// So:
//   - FetchShadow: the 32 slots, refreshed from those bits before the original
//     commit of each draw (all of them for the quad and the START variation);
//   - TextureBinding: the translation of a slot (format, layout, sampler),
//     redone only when its slot, the registry or a resolve changed;
//   - TextureEntry: the content of a (physical base, layout), read, detiled
//     and uploaded once (an engine operation of its own, UploadSampled), and
//     again only after an event: a write to its pages (the physical memory
//     watch of the SDK), the last Unlock of its header (8212DBE0, which
//     uploads it), its destruction (D750) or an own resolve. Entries are
//     registered by the D210 and the Unlock from the header's own fetch
//     constant, or by the first binding (headers built by the engine).
// The draw carries no texture data. The integral hash of every bind is taken
// only with the texture verification (TextureVerify).
#include "gpu/textures/textures.h"
#include "gpu/textures/layout.h"
#include "gpu/d3d/d3d.h"
#include "gpu/diagnostics/diagnostics.h"
#include "gpu/textures/write_watch.h"
#include "gpu/d3d/hooks.h"
#include "gpu/d3d/resources.h"
#include "gpu/d3d/device.h"
#include "gpu/d3d/frame.h"
#include "gpu/diagnostics/measure.h"
#include "gpu/diagnostics/motion.h"
#include "gpu/common/guest_memory.h"
#include "gpu/common/hash.h"
#include "gpu/settings.h"
#include "gpu/common/records.h"
#include "gpu/backend/dispatcher.h"
#include "gpu/host/services.h"
#include "gpu/diagnostics/profile.h"
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
// Texture fetch constant (6 dwords), field layout of xe_gpu_texture_fetch_t.
std::string TextureFetchFields(const std::array<uint32_t, 6>& d) {
  const uint32_t dimension = (d[5] >> 9) & 3;
  uint32_t width = 0, height = 1, depth = 1;
  if (dimension == 0) width = (d[2] & 0xFFFFFF) + 1;
  else if (dimension == 2) { width = (d[2] & 0x7FF) + 1; height = ((d[2] >> 11) & 0x7FF) + 1; depth = (d[2] >> 22) + 1; }
  else { width = (d[2] & 0x1FFF) + 1; height = ((d[2] >> 13) & 0x1FFF) + 1; depth = (d[2] >> 26) + 1; }
  auto sext = [](uint32_t v, unsigned bits) { return int32_t(v << (32 - bits)) >> (32 - bits); };
  return fmt::format(
      "\"type\":{},\"sign\":[{},{},{},{}],\"clamp\":[{},{},{}],\"signed_rf\":{},\"dim_tbd\":{},\"pitch\":{},"
      "\"tiled\":{},\"format\":{},\"endian\":{},\"request_size\":{},\"stacked\":{},\"clamp_policy\":{},\"base\":{},"
      "\"dimension\":{},\"size\":[{},{},{}],\"num_format\":{},\"swizzle\":[{},{},{},{}],\"exp_adjust\":{},"
      "\"filter\":[{},{},{},{}],\"arbitrary_filter\":{},\"border_size\":{},\"vol_filter\":[{},{}],\"mip_levels\":[{},{}],"
      "\"lod_bias\":{},\"border_color\":{},\"tri_clamp\":{},\"aniso_bias\":{},\"packed_mips\":{},\"mip_address\":{}",
      d[0] & 3, (d[0] >> 2) & 3, (d[0] >> 4) & 3, (d[0] >> 6) & 3, (d[0] >> 8) & 3, (d[0] >> 10) & 7,
      (d[0] >> 13) & 7, (d[0] >> 16) & 7, (d[0] >> 19) & 1, (d[0] >> 20) & 3, (d[0] >> 22) & 0x1FF, d[0] >> 31,
      d[1] & 0x3F, (d[1] >> 6) & 3, (d[1] >> 8) & 3, (d[1] >> 10) & 1, (d[1] >> 11) & 1, d[1] & 0xFFFFF000u,
      dimension, width, height, depth, d[3] & 1, (d[3] >> 1) & 7, (d[3] >> 4) & 7, (d[3] >> 7) & 7, (d[3] >> 10) & 7,
      sext((d[3] >> 13) & 0x3F, 6), (d[3] >> 19) & 3, (d[3] >> 21) & 3, (d[3] >> 23) & 3, (d[3] >> 25) & 7,
      (d[3] >> 28) & 7, d[3] >> 31, d[4] & 1, (d[4] >> 1) & 1, (d[4] >> 2) & 0xF, (d[4] >> 6) & 0xF,
      sext((d[4] >> 12) & 0x3FF, 10), d[5] & 3, (d[5] >> 3) & 3, sext((d[5] >> 5) & 0xF, 4), (d[5] >> 11) & 1,
      d[5] & 0xFFFFF000u);
}

struct FetchShadow;
const std::array<uint32_t, 6>& FetchShadowSlot(uint32_t reg);
// Record of the textures of a draw before the texture layer exists: samplers
// of both shaders, the fetch constant of the register (the translation's copy
// of device +0x480 + 24 * register; the translation reads no device word) and the
// registered texture headers with the same base address (their fetch dwords at
// +0x1C, CPU write count, own resolve).
std::string TexturesRecord(uint8_t* base, const BoundShader& vs, const BoundShader& ps) {
  std::string out;
  const auto textures = resources.Keys(ResourceRegistry::Kind::Texture);
  for (const auto* s : {&vs, &ps}) {
    std::vector<ShaderSampler> samplers;
    const bool table = s->object && ReadSamplers(base, *s, samplers);  // No PS in a depth-only draw.
    std::string entries;
    for (const auto& sampler : samplers) {
      std::array<uint32_t, 6> d{};
      const uint32_t at = 0x480 + 24 * sampler.reg;  // Device offset of the fetch constant.
      if (sampler.reg < 32) d = FetchShadowSlot(sampler.reg);
      const uint32_t address = d[1] & 0xFFFFF000u;
      std::string headers;
      for (uint32_t header : textures) {
        if ((Get32(base, header + 0x20) & 0xFFFFF000u) != address) continue;
        headers += fmt::format("{}{{\"header\":{},\"hex\":\"{}\",\"cpu_writes\":{},\"own_resolve\":{}}}",
                               headers.empty() ? "" : ",", header, GuestHex(base, header, 52),
                               texture_content.Writes(header), texture_content.Resolved(header));
      }
      entries += fmt::format("{}{{\"name\":\"{}\",\"reg\":{},\"type\":{},\"fetch_offset\":{},\"dwords\":[{}],{},"
                             "\"headers\":[{}],\"data_head\":\"{}\"}}", entries.empty() ? "" : ",", sampler.name,
                             sampler.reg, sampler.type, at, fmt::join(d, ","), TextureFetchFields(d), headers,
                             PhysicalHex(base, address, 64));
    }
    out += fmt::format("{}\"{}_samplers\":{{\"table\":{},\"entries\":[{}]}}", out.empty() ? "" : ",",
                       s == &vs ? "vs" : "ps", table, entries);
  }
  return out;
}

// The Xenos 2D tiling and the guest formats live in textures/formats.cpp.

// The verification of the texture layer: with the diagnostics outside the
// measurement mode, and in the measurement mode with
// REX_NATIVE_TEXTURE_VERIFY=1, every bind compares the live fetch constant
// with the copy (texture-fetch-shadow-missed-write; the title compares at the
// capture, FetchMissedWrites) and the content with its hash at the last read
// (texture-watch-missed-write).
bool TextureVerify() {
  static const bool value = [] {
    if (!Diagnostics()) return false;
    if (MeasureSwaps() <= 0) return true;
    const char* text = std::getenv("REX_NATIVE_TEXTURE_VERIFY");
    return text && std::string_view(text) == "1";
  }();
  return value;
}

// Where the levels of a texture lie (LevelPlace of the SDK's layout, layout.h)
// and the guest ranges its content reads. TexelConversion (formats.h) is
// applied after the endian swap.
struct TextureDesc {
  uint32_t block = 1, bpb = 4, log2_bpp = 2, endian = 0, row_pitch = 0;
  TexelConversion conversion = TexelConversion::kNone;
  uint32_t faces = 1;  // A cubemap's six faces, each at + face * the place's face_stride.
  bool tiled = false;
  std::vector<LevelPlace> places;
  std::vector<std::pair<uint32_t, uint32_t>> ranges;  // Each distinct place once, with its largest region.
};
// The fetch constant of a header holds guest virtual bases (0xFD610000 in the
// locks); the device copy holds the physical one, as the D3D converts
// it (8212DBE0): GuestPhysicalAlias, which keeps a physical address unchanged.
std::array<uint32_t, 6> PhysicalFetch(std::array<uint32_t, 6> d) {
  d[1] = GuestPhysicalAlias(d[1] & 0xFFFFF000u) | (d[1] & 0xFFF);
  if (d[5] & 0xFFFFF000u) d[5] = GuestPhysicalAlias(d[5] & 0xFFFFF000u) | (d[5] & 0xFFF);
  return d;
}
// vertex: a vertex texture, fetch constants 16 to 19 (the shared constants
// take its indices at c18/c19, build-sdb-shader-cache.py); a 1D texture (the
// bone matrices of the Story Mode skinned meshes) is covered there only,
// linear with one level, as a 2D texture one texel high (tfetch1D).
// The SDK's host swizzle of a format whose host texture lacks components
// (host_formats_ of its texture cache): RRRR for k_8 and k_32_FLOAT, RGGG for
// k_32_32_FLOAT and DXN. It acts under the fetch swizzle: a component selector
// past the last host component takes that component, the constants stay. The
// host swizzle is given as the four host components (formats.h).
ComponentMapping HostSwizzledMapping(ComponentMapping mapping, const uint8_t (&host)[4]) {
  for (auto& swizzle : mapping)
    if (swizzle < Swizzle::kZero) swizzle = Swizzle(host[uint32_t(swizzle)]);
  return mapping;
}
// The fetch swizzle (3 bits per component, selectors x, y, z, w, 0, 1) as the
// component mapping; selectors above 5 are refused before.
ComponentMapping FetchMapping(uint32_t swizzle) {
  ComponentMapping mapping;
  for (unsigned i = 0; i < 4; ++i) mapping[i] = Swizzle((swizzle >> (3 * i)) & 7);
  return mapping;
}
// The translation of a fetch constant without its content: the request
// fields (sampler state included), the layout key and the places.
bool DecodeTexture(uint8_t* base, const std::array<uint32_t, 6>& d, uint32_t reg, bool vertex, DrawTexture& t,
                   TextureDesc& desc, std::string& reason, std::string& record, bool want_record) {
  const uint32_t format = d[1] & 0x3F, endian = (d[1] >> 6) & 3, address = d[1] & 0xFFFFF000u;
  const uint32_t dimension = (d[5] >> 9) & 3, swizzle = (d[3] >> 1) & 0xFFF;
  const bool one_d = vertex && dimension == 0;
  // A cubemap of the pixel shader (g_EnvTextureSampler of the Story Mode scene).
  const bool cube = !vertex && dimension == 3;
  const uint32_t width = one_d ? (d[2] & 0xFFFFFF) + 1 : (d[2] & 0x1FFF) + 1;
  const uint32_t height = one_d ? 1 : ((d[2] >> 13) & 0x1FFF) + 1;
  const uint32_t mip_min = (d[4] >> 2) & 0xF, mip_max = (d[4] >> 6) & 0xF;
  if (want_record) record = fmt::format("\"reg\":{},\"dwords\":[{}],{}", reg, fmt::join(d, ","), TextureFetchFields(d));
  if ((d[0] & 3) != 2) { reason = "texture-fetch-type"; return false; }
  if ((dimension != 1 && !one_d && !cube) || ((d[1] >> 10) & 1)) { reason = "texture-dimension-uncovered"; return false; }
  if (one_d && ((d[0] >> 31) || mip_max)) { reason = "texture-1d-layout-uncovered"; return false; }
  const GuestFormat* host = FindGuestFormat(format);
  for (unsigned i = 0; i < 4; ++i)
    if (((swizzle >> (3 * i)) & 7) > 5) { reason = "texture-swizzle-uncovered"; return false; }
  // Signs and the exponent stay a guard (the game's assets carry none).
  // Per result component, the sign of the guest component it reads (the
  // swizzled signs, as the SDK's shader translator applies them,
  // dxbc_translator_fetch.cpp): a sign no result reads changes nothing. A signed
  // component of a sign-free format (float and depth: the SDK's signed SRV is
  // the unsigned one; the bone matrices, the depth resolve)
  // reads the unsigned data. The number format (d[3] bit 0) is not used for
  // textures by the SDK (only vertex fetch and memexport), so not a guard.
  for (unsigned i = 0; i < 4; ++i) {
    const uint32_t select = (swizzle >> (3 * i)) & 7;
    const uint32_t sign = select < 4 ? (d[0] >> (2 + 2 * select)) & 3 : 0;
    if (sign && !(sign == 1 && host && host->sign_free)) { reason = "texture-sign-uncovered"; return false; }
  }
  if ((d[3] >> 13) & 0x3F) { reason = "texture-sign-uncovered"; return false; }
  t.reg = reg;
  t.width = width;
  t.height = height;
  // The Xenos swizzle selectors (x, y, z, w, 0, 1) are the swizzles, in order.
  t.component_mapping = FetchMapping(swizzle);
  // Sampler state of the fetch constant.
  const uint32_t mag = (d[3] >> 19) & 3, min = (d[3] >> 21) & 3, mip = (d[3] >> 23) & 3, aniso = (d[3] >> 25) & 7;
  if (aniso == 7 || aniso > 5 || mag > 1 || min > 1 || mip > 2) { reason = "texture-filter-uncovered"; return false; }
  // The mip filter base map (2) samples the base level as point (max_lod below).
  const auto filter = [](uint32_t linear) { return linear ? Filter::kLinear : Filter::kPoint; };
  t.min_filter = filter(min);
  t.mag_filter = filter(mag);
  t.mip_filter = filter(mip & 1);
  t.anisotropic = aniso != 0;
  t.max_anisotropy = aniso ? 1u << (aniso - 1) : 1;
  // anisotropic_override (SDK levels: 0 off, 1 to 5 for 1x to 16x) replaces
  // the title's filter only with a linear mip filter.
  if (const int32_t forced = CurrentSettings().anisotropic_override; forced >= 0 && mip == 1) {
    t.anisotropic = forced != 0;
    t.mip_filter = Filter::kLinear;
    t.max_anisotropy = forced ? 1u << (forced - 1) : 1;
    ++texture_anisotropy_forced;
  }
  // The eight Xenos clamp modes: halfway clamps take the plain clamp of the
  // same mirroring, clamp to border is BORDER and mirror clamp to border
  // MIRROR_ONCE. Border color: ABGR white is opaque white, the others
  // transparent black.
  using A = AddressMode;
  const AddressMode modes[] = {A::kWrap, A::kMirror, A::kClamp, A::kMirrorOnce,
                               A::kClamp, A::kMirrorOnce, A::kBorder, A::kMirrorOnce};
  t.address_u = modes[(d[0] >> 10) & 7];
  t.address_v = modes[(d[0] >> 13) & 7];
  t.address_w = A::kClamp;
  t.border_color = (d[5] & 3) == 1 ? std::array<float, 4>{1, 1, 1, 1} : std::array<float, 4>{0, 0, 0, 0};
  t.lod_bias = float(int32_t(((d[4] >> 12) & 0x3FF) << 22) >> 22) / 32.0f;
  t.min_lod = float(mip_min);
  t.max_lod = float(mip == 2 ? mip_min : mip_max);
  // Own resolve of a header at this base address. 8_8_8_8: the own copy holds
  // the rendered channels (identity mapping). k_8 (the per-character
  // maps of the Story Mode scene; the own copy keeps the RGBA8 of the source)
  // and the depth formats (R32_FLOAT of the depth resolve pass) read as the
  // Xenos host swizzle RRRR under the fetch swizzle: every component selector
  // takes R, the constants stay.
  if (const uint32_t header = texture_content.ResolvedAt(address)) {
    const uint32_t hw = Get32(base, header + 0x24);
    const bool depth_dest = format == 22 || format == 23;
    if ((format != 6 && format != 2 && !depth_dest) || width != (hw & 0x1FFF) + 1 || height != ((hw >> 13) & 0x1FFF) + 1 ||
        cube) {
      reason = "texture-resolved-uncovered";
      return false;
    }
    t.resolved = true;
    t.key = header;
    if (format == 6) {
      t.component_mapping = kIdentityMapping;
    } else {
      for (auto& component : t.component_mapping)
        if (component < Swizzle::kZero) component = Swizzle::kR;
    }
    t.format = depth_dest ? TextureFormat::kR32Float : TextureFormat::kR8G8B8A8Unorm;
    t.min_lod = t.max_lod = 0;
    if (want_record) record += fmt::format(",\"resolved_header\":{},\"resolved_format\":{}", header, format);
    return true;
  }
  // The host format of the SDK's table (formats.cpp), for the 13 formats the
  // game uses. The SDK's host swizzle (RRRR, RGGG, RGBB) acts under the fetch
  // swizzle. The endian swap goes by the element size as in the SDK
  // (ReadGuestLevel).
  if (!host) { reason = "texture-format-uncovered"; return false; }
  // A block-compressed texture whose base is not a
  // multiple of 4 texels takes the SDK's decompressed host format.
  if (IsBlockCompressed(host->host) && ((width & 3) || (height & 3))) {
    host = FindGuestFormat(format, true);
    if (!host) { reason = "texture-block-size-uncovered"; return false; }
  }
  t.format = host->host;
  t.component_mapping = HostSwizzledMapping(t.component_mapping, host->swizzle);
  const uint32_t block = host->block, bpb = host->bytes_per_block;
  const TexelConversion conversion = host->conversion;
  const bool tiled = d[0] >> 31;
  // What the engine refuses (Engine::UploadSampled) stays
  // a guard of the binding, never a failure of a posted upload (a header the
  // Unlock uploads before any draw binds it).
  if (width > 8192 || height > 8192 || mip_max > 12 || (cube && width != height)) {
    reason = "texture-size-uncovered";
    return false;
  }
  const uint32_t log2_bpp = bpb == 1 ? 0 : bpb == 2 ? 1 : bpb == 4 ? 2 : bpb == 8 ? 3 : 4;
  // The place of every level and face from the SDK's
  // layout (layout.cpp): mip chains of any size, linear mips, the packed tail
  // (the base too) at its block offsets, cubes with mips. The SDK clamps the
  // mip range (no mip page: the base only; the last level by the longest axis).
  GuestTextureLayout layout;
  if (!GetTextureLayout(d, layout, reason)) return false;
  if (!layout.has_base) { reason = "texture-base-uncovered"; return false; }
  if (!one_d && !((d[0] >> 22) & 0x1FF)) { reason = "texture-pitch-uncovered"; return false; }
  t.levels = layout.mip_max + 1;
  t.min_lod = float(layout.mip_min);
  t.max_lod = float(((d[3] >> 23) & 3) == 2 ? layout.mip_min : layout.mip_max);
  if (cube) t.cube = true;
  auto& places = desc.places;
  places = layout.levels;
  for (const LevelPlace& place : places) {
    const uint32_t span = (layout.faces - 1) * place.face_stride + place.region_bytes;
    if (place.address >= 0x20000000u || span > 0x20000000u - place.address || span > 0x1000000) {
      reason = "texture-address-range";
      return false;
    }
  }
  std::map<uint32_t, uint32_t> regions;
  for (const LevelPlace& place : places)
    regions[place.address] = std::max(regions[place.address], (layout.faces - 1) * place.face_stride + place.region_bytes);
  desc.ranges.assign(regions.begin(), regions.end());
  desc.faces = layout.faces;
  desc.block = block;
  desc.bpb = bpb;
  desc.log2_bpp = log2_bpp;
  desc.endian = endian;
  desc.conversion = conversion;
  desc.tiled = tiled;
  const uint32_t layout_words[] = {d[0] & 0xFFC00000u, d[1], d[2], swizzle, d[4] & 0x3FC, d[5] & 0xFFFFFE00u};
  t.layout = Fnv64(reinterpret_cast<const uint8_t*>(layout_words), sizeof(layout_words));
  t.key = address;
  if (want_record) {
    std::string places_record;
    for (uint32_t level = 0; level < t.levels; ++level)
      places_record += fmt::format("{}{{\"address\":{},\"row_pitch\":{},\"offset_blocks\":[{},{}],\"bytes\":{},"
                                   "\"face_stride\":{}}}", level ? "," : "", places[level].address,
                                   places[level].row_pitch_bytes, places[level].x0_blocks, places[level].y0_blocks,
                                   places[level].region_bytes, places[level].face_stride);
    record += fmt::format(",\"layout\":{},\"levels\":[{}],\"faces\":{}", t.layout, places_record, desc.faces);
  }
  return true;
}
// The levels of a decoded texture, detiled from guest physical memory; a
// cubemap face after face (the D3D12 subresource order: level + face * levels).
bool ReadTextureLevels(uint8_t* base, const DrawTexture& t, const TextureDesc& desc,
                       std::vector<std::vector<uint8_t>>& data, std::string& reason) {
  const uint8_t* physical = base + 0x100000000ull;
  const uint32_t block = desc.block, bpb = desc.bpb;
  data.clear();
  for (uint32_t face = 0; face < desc.faces; ++face) {
    for (uint32_t level = 0; level < t.levels; ++level) {
      const LevelPlace& place = desc.places[level];
      const uint32_t face_address = place.address + face * place.face_stride;
      const uint32_t lw = std::max(t.width >> level, 1u), lh = std::max(t.height >> level, 1u);
      const uint32_t bw = (lw + block - 1) / block, bh = (lh + block - 1) / block;
      // Detiling, the endian swap and the conversion of formats.cpp, the
      // code the host test runs against the SDK's load shaders, at the place
      // of the SDK's layout (layout.cpp).
      GuestLevel guest;
      guest.memory = physical + face_address;
      guest.region_bytes = place.region_bytes;
      guest.pitch_blocks = place.row_pitch_bytes / bpb;
      guest.row_pitch_bytes = place.row_pitch_bytes;
      guest.tiled = desc.tiled;
      guest.endian = desc.endian;
      guest.x0_blocks = place.x0_blocks;
      guest.y0_blocks = place.y0_blocks;
      guest.width_blocks = bw;
      guest.height_blocks = bh;
      guest.bytes_per_block = bpb;
      auto& out = data.emplace_back();
      if (!ReadGuestLevel(guest, out)) { reason = "texture-tile-range"; return false; }
      ConvertLevel(desc.conversion, out, bw, bh, lw, lh);
    }
  }
  return true;
}
uint64_t TextureContentHash(uint8_t* base, const TextureDesc& desc) {
  profile::Timer hash_time(profile::kDrawTextureHash);
  uint64_t h = 0xCBF29CE484222325ull, bytes = 0;
  for (const auto& [at, n] : desc.ranges) {
    h = FastHash64(base + 0x100000000ull + at, n, h);
    bytes += n;
  }
  profile::Count(profile::kHashedBytes, bytes);
  return h;
}

// The engine's sampled copy of one physical base (Engine keys it
// by the base). An upload reads, posts and notes under the mutex, so the posts
// of a base follow their versions; the D750 releases under it.
struct TextureSlot {
  std::mutex mutex;
  uint64_t layout = 0, version = 0;  // Posted to the engine (FIFO), under mutex.
  std::atomic<uint64_t> posted_layout{0}, posted_version{0};  // The same, for the draw's check without the lock.
};
struct TextureEntry {
  DrawTexture t;  // Format, size, levels, key (the base) and layout; no data.
  TextureDesc desc;
  std::shared_ptr<TextureSlot> slot;
  // Content, written under slot->mutex; the atomics are also read by the draw.
  // A version is published only once its upload is posted, so a draw that
  // binds it is recorded after the copy (FIFO).
  std::atomic<uint64_t> version{0};  // 0: never read; else unique (texture_versions).
  std::atomic<uint64_t> checked_epoch{0};  // Watch epoch the content was last found unchanged at.
  std::atomic<uint64_t> chunk_sum{0};      // Watch counts of its 1 MB chunks at the last read.
  uint64_t page_sum = 0;                   // And of its pages.
  uint64_t integral = 0;                   // Content hash at the last read (verification only).
  bool header = false;             // Registered from a header (D210 or Unlock).
  std::atomic<bool> bound{false};
};
std::atomic<uint64_t> texture_versions{0};
struct TextureKey {
  uint32_t address;
  uint64_t layout;
  bool operator==(const TextureKey& o) const { return address == o.address && layout == o.layout; }
};
struct TextureKeyHash {
  size_t operator()(const TextureKey& k) const { return size_t(k.layout ^ (uint64_t(k.address) * 0x9E3779B97F4A7C15ull)); }
};
class TextureRegistry {
 public:
  std::shared_ptr<TextureEntry> FindOrCreate(const DrawTexture& t, const TextureDesc& desc, bool header) {
    std::lock_guard lock(mutex_);
    auto& entry = entries_[{t.key, t.layout}];
    if (!entry) {
      entry = std::make_shared<TextureEntry>();
      entry->t = t;
      entry->t.data.clear();
      entry->desc = desc;
      auto& slot = slots_[t.key];
      if (!slot) slot = std::make_shared<TextureSlot>();
      entry->slot = slot;
      entry->header = header;
      ++(header ? from_headers_ : from_bindings_);
      profile::Count(profile::kTexturesRegistered, 1);
      if (forgotten_bases_.erase(t.key)) {  // A base registered again after its D750.
        ++recreated_;
        profile::Count(profile::kTexturesRecreated, 1);
      }
    }
    return entry;
  }
  std::shared_ptr<TextureEntry> Find(uint32_t address, uint64_t layout) {
    std::lock_guard lock(mutex_);
    auto it = entries_.find({address, layout});
    return it == entries_.end() ? nullptr : it->second;
  }
  std::shared_ptr<TextureSlot> Slot(uint32_t address) {
    std::lock_guard lock(mutex_);
    auto it = slots_.find(address);
    return it == slots_.end() ? nullptr : it->second;
  }
  // D750: the entries of the base leave the registry, and the bindings that
  // hold one are translated again (generation); the slot goes to the release.
  std::shared_ptr<TextureSlot> Forget(uint32_t address) {
    std::lock_guard lock(mutex_);
    forgotten_ += std::erase_if(entries_, [address](const auto& e) { return e.first.address == address; });
    auto it = slots_.find(address);
    if (it == slots_.end()) return nullptr;
    auto slot = std::move(it->second);
    slots_.erase(it);
    generation_.fetch_add(1, std::memory_order_release);
    forgotten_bases_.insert(address);
    ++forgotten_base_calls_;
    profile::Count(profile::kTexturesForgotten, 1);
    return slot;
  }
  uint64_t Generation() const { return generation_.load(std::memory_order_acquire); }
  // The bases with an engine copy, and the entries registered and the
  // bases forgotten since the start (the scene changes of the transition captures).
  struct Totals {
    uint64_t bases = 0, registered = 0, forgotten_bases = 0;
  };
  Totals Snapshot() {
    std::lock_guard lock(mutex_);
    return {slots_.size(), from_headers_ + from_bindings_, forgotten_base_calls_};
  }
  std::string Summary() {
    std::lock_guard lock(mutex_);
    return fmt::format("\"entries\":{},\"bases\":{},\"from_headers\":{},\"from_bindings\":{},\"forgotten\":{},"
                       "\"generation\":{},\"forgotten_bases\":{},\"recreated\":{}", entries_.size(), slots_.size(),
                       from_headers_, from_bindings_, forgotten_, generation_.load(), forgotten_base_calls_, recreated_);
  }
 private:
  std::mutex mutex_;
  std::unordered_map<TextureKey, std::shared_ptr<TextureEntry>, TextureKeyHash> entries_;
  std::unordered_map<uint32_t, std::shared_ptr<TextureSlot>> slots_;
  std::unordered_set<uint32_t> forgotten_bases_;  // Bases forgotten by a D750 and not registered since.
  std::atomic<uint64_t> generation_{1};
  uint64_t from_headers_ = 0, from_bindings_ = 0, forgotten_ = 0, forgotten_base_calls_ = 0, recreated_ = 0;
} texture_registry;
std::atomic<uint64_t> texture_uploads{0}, texture_reads{0}, texture_upload_bytes{0}, texture_binding_translations{0},
    texture_binding_reuses{0}, texture_header_hits{0}, texture_verifies{0}, texture_header_registrations{0},
    texture_header_uncovered{0}, texture_unlocks{0}, texture_unlock_uploads{0}, texture_unlock_uncovered{0},
    texture_anisotropy_forced{0};
// Under e.slot->mutex: reads the content again when it was never read, a
// write reached its pages or force (an Unlock), and posts it when the engine's
// copy of the base is not this version. False: reason.
bool UploadTexture(uint8_t* base, TextureEntry& e, bool force, std::string& reason) {
  TextureSlot& slot = *e.slot;
  const uint64_t epoch = write_watch::epoch.load(std::memory_order_acquire);
  uint64_t version = e.version.load();
  bool read = force || !version;
  if (!read && e.checked_epoch.load() != epoch) {
    const uint64_t chunks = write_watch::Sum(e.desc.ranges, false);
    if (chunks != e.chunk_sum.load()) {
      read = write_watch::Sum(e.desc.ranges, true) != e.page_sum;
      if (!read) e.chunk_sum.store(chunks);
    }
  }
  if (!read && slot.layout == e.t.layout && slot.version == version) {
    e.checked_epoch.store(epoch);
    return true;
  }
  DrawTexture t = e.t;
  uint64_t chunks = 0, pages = 0;
  if (read) {  // The counts before the arm: a write after them is seen by the read or counted.
    chunks = write_watch::Sum(e.desc.ranges, false);
    pages = write_watch::Sum(e.desc.ranges, true);
    write_watch::Arm(e.desc.ranges);
  }
  if (!ReadTextureLevels(base, e.t, e.desc, t.data, reason)) return false;
  if (read) {
    e.chunk_sum.store(chunks);
    e.page_sum = pages;
    if (TextureVerify()) e.integral = TextureContentHash(base, e.desc);
    version = ++texture_versions;
    ++texture_reads;
  }
  t.version = version;
  uint64_t bytes = 0;
  for (const auto& level : t.data) bytes += level.size();
  auto* dispatcher = host::ActiveDispatcher();
  if (!dispatcher || !dispatcher->Post([posted = std::move(t)](Engine& engine, std::string& out) {
        return engine.UploadSampled(posted, out); })) {
    reason = "texture-upload-unproved";
    return false;
  }
  e.version.store(version, std::memory_order_release);
  e.checked_epoch.store(epoch);
  slot.layout = e.t.layout;
  slot.version = version;
  slot.posted_layout.store(slot.layout, std::memory_order_release);
  slot.posted_version.store(slot.version, std::memory_order_release);
  ++texture_uploads;
  texture_upload_bytes += bytes;
  profile::Count(profile::kTextureUploads, 1);
  return true;
}

// The fetch constants of the 32 slots as the last draw commit left
// them, in two copies. The title's (fetch_sent) decides what changed and
// goes into the capture; the translation's (fetch_shadow, with the version of
// each slot) is the one the bindings and the streams read. Both move in step:
// every capture reaches ApplyFetch, skipped draws included.
struct FetchShadow {
  std::array<std::array<uint32_t, 6>, 32> dwords{};
  std::array<uint64_t, 32> version{};  // Unique per change (fetch_versions).
  bool valid = false;
};
FetchShadow fetch_sent;    // Title thread only.
FetchShadow fetch_shadow;  // Translation only (the render thread).
std::atomic<uint64_t> fetch_versions{0}, fetch_refreshes{0}, fetch_full_refreshes{0}, fetch_slot_changes{0};
void CaptureFetch(uint8_t* base, uint32_t dev, uint32_t mask, bool full, FetchDelta& out) {
  if (!fetch_sent.valid) full = true;
  if (!full && !mask) return;
  out.refresh = true;
  out.full = full;
  for (unsigned k = 0; k < 32; ++k) {
    if (!full && !((mask >> (31 - k)) & 1)) continue;
    std::array<uint32_t, 6> d;
    for (unsigned i = 0; i < 6; ++i) d[i] = Get32(base, dev + 0x480 + 24 * k + 4 * i);
    if (fetch_sent.valid && d == fetch_sent.dwords[k]) continue;
    fetch_sent.dwords[k] = d;
    out.dwords[k] = d;
    out.changed |= 1u << (31 - k);
  }
  fetch_sent.valid = true;
}
void ApplyFetch(const FetchDelta& delta) {
  if (!delta.refresh) return;
  for (unsigned k = 0; k < 32; ++k) {
    if (!((delta.changed >> (31 - k)) & 1)) continue;
    fetch_shadow.dwords[k] = delta.dwords[k];
    fetch_shadow.version[k] = ++fetch_versions;
    ++fetch_slot_changes;
  }
  fetch_shadow.valid = true;
  ++fetch_refreshes;
  if (delta.full) ++fetch_full_refreshes;
}
const std::array<uint32_t, 6>& FetchConstantSlot(uint32_t slot) {
  static const std::array<uint32_t, 6> none{};
  return slot < 32 ? fetch_shadow.dwords[slot] : none;
}
const std::array<uint32_t, 6>& FetchShadowSlot(uint32_t reg) { return FetchConstantSlot(reg); }
uint32_t FetchMissedWrites(uint8_t* base, uint32_t dev) {
  uint32_t missed = 0;
  for (unsigned k = 0; k < 32; ++k)
    for (unsigned i = 0; i < 6; ++i)
      if (Get32(base, dev + 0x480 + 24 * k + 4 * i) != fetch_sent.dwords[k][i]) {
        missed |= 1u << k;
        break;
      }
  return missed;
}
// The translation of a slot, kept while the slot, the registry and the own
// resolves hold.
struct TextureBinding {
  bool valid = false, vertex = false, ok = false;
  uint64_t fetch_version = 0, registry_generation = 0, resolve_generation = 0;
  const char* reason = "";
  DrawTexture t;
  std::shared_ptr<TextureEntry> entry;  // Null when resolved or uncovered.
};
std::array<TextureBinding, 32> texture_bindings;  // Render thread only.
// The translations by fetch constant, so a slot that moves between textures
// already seen takes the translation instead of decoding it again. A heavy
// frame binds ~3,000 textures from a few hundred fetch constants. The
// translation depends on the six words, the stage and the two generations of the slot
// binding; a change of generation empties the map.
struct TranslationKey {
  std::array<uint32_t, 6> d;
  bool vertex;
  bool operator==(const TranslationKey&) const = default;
};
struct TranslationKeyHash {
  size_t operator()(const TranslationKey& k) const {
    uint64_t h = k.vertex ? 0x9E3779B97F4A7C15ull : 0;
    for (uint32_t w : k.d) h = (h ^ w) * 0x100000001B3ull + (h >> 29);
    return size_t(h ^ (h >> 32));
  }
};
struct TranslationCache {
  uint64_t registry_generation = UINT64_MAX, resolve_generation = UINT64_MAX;
  std::unordered_map<TranslationKey, TextureBinding, TranslationKeyHash> map;
} texture_translations;  // Render thread only.
constexpr size_t kTranslationsMax = 8192;
std::atomic<uint64_t> texture_binding_cached{0};
// The draw's texture of one sampler: the binding of its slot (translated when
// the slot changed), the entry's content uploaded if an event changed it, and
// no data in the request. False: reason (TextureRecord rebuilds the record).
bool BindTexture(uint8_t* base, const ShaderSampler& sampler, bool vertex, bool fetch_missed, DrawTexture& out,
                 std::string& reason) {
  const uint32_t reg = sampler.reg;
  if (vertex ? reg < 16 || reg > 19 : reg >= 16) {
    reason = "texture-register-uncovered";
    return false;
  }
  auto& b = texture_bindings[reg];
  static int32_t filtering = -2;
  const int32_t requested_filter = CurrentSettings().anisotropic_override;
  if (filtering != requested_filter) {
    // Both caches contain decoded sampler state, including the override.
    texture_bindings = {};
    texture_translations.map.clear();
    filtering = requested_filter;
  }
  const uint64_t generation = texture_registry.Generation(), resolves = texture_content.ResolveGeneration();
  if (!b.valid || b.vertex != vertex || b.fetch_version != fetch_shadow.version[reg] ||
      b.registry_generation != generation || b.resolve_generation != resolves) {
    auto& cache = texture_translations;
    if (cache.registry_generation != generation || cache.resolve_generation != resolves ||
        cache.map.size() >= kTranslationsMax) {
      cache.map.clear();
      cache.registry_generation = generation;
      cache.resolve_generation = resolves;
    }
    const TranslationKey key{fetch_shadow.dwords[reg], vertex};
    if (const auto cached = cache.map.find(key); cached != cache.map.end()) {
      b = cached->second;
      b.t.reg = reg;
      b.fetch_version = fetch_shadow.version[reg];
      ++texture_binding_cached;
    } else {
      b = TextureBinding{};
      b.valid = true;
      b.vertex = vertex;
      b.fetch_version = fetch_shadow.version[reg];
      b.registry_generation = generation;
      b.resolve_generation = resolves;
      TextureDesc desc;
      std::string why, record;
      b.ok = DecodeTexture(base, fetch_shadow.dwords[reg], reg, vertex, b.t, desc, why, record, false);
      if (!b.ok) {
        static std::mutex reasons_mutex;  // Stable storage of the few reason strings.
        static std::unordered_set<std::string> reasons;
        std::lock_guard lock(reasons_mutex);
        b.reason = reasons.insert(why).first->c_str();
      } else if (!b.t.resolved) {
        b.entry = texture_registry.FindOrCreate(b.t, desc, false);
        if (b.entry->header && !b.entry->bound.load()) ++texture_header_hits;
      }
      ++texture_binding_translations;
      cache.map.emplace(key, b);
    }
  } else {
    ++texture_binding_reuses;
  }
  if (!b.ok) {
    reason = b.reason;
    return false;
  }
  if (TextureVerify()) {
    ++texture_verifies;
    if (fetch_missed) {
      reason = "texture-fetch-shadow-missed-write";
      return false;
    }
  }
  out = b.t;
  if (vertex && motion::Enabled()) motion::NoteVertexTexture(fetch_shadow.dwords[reg]);  // The bone palettes.
  if (b.t.resolved) return true;
  TextureEntry& e = *b.entry;
  e.bound.store(true, std::memory_order_relaxed);
  // Without the lock: the posted copy is this version (versions are unique)
  // and no write reached its chunks since the last read.
  uint64_t version = e.version.load(std::memory_order_acquire);
  bool current = version && e.slot->posted_version.load(std::memory_order_acquire) == version;
  if (current) {
    const uint64_t epoch = write_watch::epoch.load(std::memory_order_acquire);
    if (e.checked_epoch.load(std::memory_order_acquire) != epoch) {
      if (write_watch::Sum(e.desc.ranges, false) == e.chunk_sum.load(std::memory_order_acquire))
        e.checked_epoch.store(epoch, std::memory_order_release);
      else
        current = false;
    }
  }
  if (!current) {
    std::lock_guard lock(e.slot->mutex);
    if (!UploadTexture(base, e, false, reason)) return false;
    version = e.version.load();
  }
  out.version = version;
  if (TextureVerify()) {
    std::lock_guard lock(e.slot->mutex);
    const uint64_t epoch = write_watch::epoch.load(std::memory_order_acquire);
    if (e.checked_epoch.load() == epoch && TextureContentHash(base, e.desc) != e.integral &&
        write_watch::epoch.load(std::memory_order_acquire) == epoch) {
      reason = "texture-watch-missed-write";
      return false;
    }
  }
  return true;
}
// The record of a sampler's texture (the diagnostics and the guards).
std::string TextureRecord(uint8_t* base, uint32_t reg, bool vertex) {
  if (reg >= 32) return fmt::format("\"reg\":{}", reg);
  DrawTexture t;
  TextureDesc desc;
  std::string why, record;
  DecodeTexture(base, fetch_shadow.dwords[reg], reg, vertex, t, desc, why, record, true);
  const auto& b = texture_bindings[reg];
  const TextureEntry* e = b.entry.get();
  return record + fmt::format(",\"binding\":{{\"fetch_version\":{},\"ok\":{},\"reason\":\"{}\"}},\"entry\":{{\"known\":{},"
                              "\"version\":{},\"header\":{},\"posted_version\":{}}}", b.fetch_version, b.ok, b.reason,
                              e != nullptr, e ? e->version.load() : 0, e ? e->header : false,
                              e ? e->slot->posted_version.load() : 0);
}
// D210: the header is registered by its own fetch constant (+0x1C), without
// reading its content; a layout outside the covered ones is counted here and
// guarded when a draw binds it.
void RegisterTextureHeader(uint8_t* base, uint32_t header) {
  std::array<uint32_t, 6> d;
  for (unsigned i = 0; i < 6; ++i) d[i] = Get32(base, header + 0x1C + 4 * i);
  if (!(d[1] & 0xFFFFF000u)) return;  // No memory yet.
  d = PhysicalFetch(d);
  DrawTexture t;
  TextureDesc desc;
  std::string reason, record;
  if (!DecodeTexture(base, d, 0, ((d[5] >> 9) & 3) == 0, t, desc, reason, record, false)) {
    ++texture_header_uncovered;
    return;
  }
  if (t.resolved) return;
  texture_registry.FindOrCreate(t, desc, true);
  ++texture_header_registrations;
}
// The last Unlock (8212DBE0) of a resource: the CPU-written ranges it
// flushes, (first << 16 | last) in 128-byte lines from the base (+0x14) and
// the mip address (+0x18), are a write event of their pages; a texture is
// then uploaded: the layout the engine holds for
// its base (the one the draws bind), else the header's own.
void NoteUnlock(PPCContext& ctx, uint8_t* base, uint32_t resource, uint32_t common, uint32_t base_address,
                uint32_t lines, uint32_t mip_address, uint32_t mip_lines) {
  auto written = [](uint32_t address, uint32_t l) {
    if (!address || l == 0xFFFF0000u) return;
    const uint32_t first = (l >> 16) << 7, last = (l & 0xFFFF) << 7;
    if (last < first) return;
    write_watch::Bump(GuestPhysicalAlias(address) + first, last - first + 128);
    ++write_watch::unlock_ranges;
  };
  written(base_address, lines);
  written(mip_address, mip_lines);
  if ((common & 0xF) != 3) return;
  ++texture_unlocks;
  if (!device_owner || !device_owner->live()) return;
  std::array<uint32_t, 6> d;
  for (unsigned i = 0; i < 6; ++i) d[i] = Get32(base, resource + 0x1C + 4 * i);
  if (!(d[1] & 0xFFFFF000u)) return;
  d = PhysicalFetch(d);
  DrawTexture t;
  TextureDesc desc;
  std::string reason, record;
  if (!DecodeTexture(base, d, 0, ((d[5] >> 9) & 3) == 0, t, desc, reason, record, false)) {
    ++texture_unlock_uncovered;
    return;
  }
  if (t.resolved) return;
  // The upload in order with the captures before this Unlock (on the stage
  // when deferred: the engine refuses a draw whose texture version is newer
  // than its copy, so the version must advance after those draws were posted;
  // the content is read there, since the title writes texture memory only
  // inside a lock). Inline, the same work runs here.
  auto upload = [base, t, desc]() -> std::string {
    std::shared_ptr<TextureEntry> e;
    if (const auto slot = texture_registry.Slot(t.key)) {
      uint64_t layout = 0;
      {
        std::lock_guard lock(slot->mutex);
        layout = slot->layout;
      }
      if (layout) e = texture_registry.Find(t.key, layout);
    }
    if (!e) e = texture_registry.FindOrCreate(t, desc, true);
    std::lock_guard lock(e->slot->mutex);
    std::string why;
    if (UploadTexture(base, *e, true, why)) {
      ++texture_unlock_uploads;
      return std::string();
    }
    ++texture_unlock_uncovered;  // A content outside the covered layouts is guarded when a draw binds it.
    return why;
  };
  auto* dispatcher = host::ActiveDispatcher();
  if (dispatcher && dispatcher->Deferred() && !dispatcher->OnTranslator()) {
    dispatcher->Defer([upload] {
      if (upload() == "texture-upload-unproved") {
        RequestStop("DBE0-texture-upload-unproved", 88);
        throw TranslationStopped{};
      }
    });
    return;
  }
  if (upload() == "texture-upload-unproved") StopInOrder(ctx, "DBE0-texture-upload-unproved", 88);
}
// A destroyed header's base leaves the registry, and the
// engine releases its copy (at once or retired with the fence of its last
// use). True if no copy was posted or the release ran.
bool ReleaseTextureBase(uint32_t address, std::string& evidence) {
  // The engine release goes first (through the stage, after the captures
  // that still bind the base); the registry forgets the base once it ran, so
  // no capture in between creates a new entry for memory the title frees.
  bool posted = false;
  if (const auto slot = texture_registry.Slot(address)) {
    std::lock_guard lock(slot->mutex);
    posted = slot->layout != 0;
  }
  if (posted) {
    auto* dispatcher = host::ActiveDispatcher();
    if (!dispatcher || !dispatcher->Run([address](Engine& engine, std::string& out) {
          return engine.ReleaseSampled(address, out); }, evidence))
      return false;
  }
  const auto slot = texture_registry.Forget(address);
  if (!slot) return true;
  std::lock_guard lock(slot->mutex);
  slot->layout = slot->version = 0;
  slot->posted_layout.store(0);
  slot->posted_version.store(0);
  return true;
}
// The copy posted for a base (START evidence).
bool TexturePosted(uint32_t address, uint64_t& layout, uint64_t& version) {
  const auto slot = texture_registry.Slot(address);
  if (!slot) return false;
  layout = slot->posted_layout.load();
  version = slot->posted_version.load();
  return layout != 0;
}
TextureRegistryTotals TextureRegistrySnapshot() {
  const auto totals = texture_registry.Snapshot();
  return {totals.bases, totals.registered, totals.forgotten_bases};
}
std::string TextureRegistrySummary() {
  return texture_registry.Summary() + fmt::format(
      ",\"uploads\":{},\"reads\":{},\"upload_bytes\":{},\"binding_translations\":{},\"binding_reuses\":{},\"binding_cached\":{},"
      "\"header_hits\":{},\"header_registrations\":{},\"header_uncovered\":{},\"unlocks\":{},\"unlock_uploads\":{},"
      "\"unlock_uncovered\":{},\"verifies\":{},\"fetch\":{{\"refreshes\":{},\"full\":{},\"slot_changes\":{}}},"
      "\"watch\":{{\"epoch\":{},\"notifications\":{},\"notified_pages\":{},\"unlock_ranges\":{},\"armed_ranges\":{}}}",
      texture_uploads.load(), texture_reads.load(), texture_upload_bytes.load(), texture_binding_translations.load(),
      texture_binding_reuses.load(), texture_binding_cached.load(), texture_header_hits.load(), texture_header_registrations.load(),
      texture_header_uncovered.load(), texture_unlocks.load(), texture_unlock_uploads.load(),
      texture_unlock_uncovered.load(), texture_verifies.load(), fetch_refreshes.load(), fetch_full_refreshes.load(),
      fetch_slot_changes.load(), write_watch::epoch.load(), write_watch::notifications.load(),
      write_watch::notified_pages.load(), write_watch::unlock_ranges.load(), write_watch::armed_ranges.load());
}
}  // namespace gpu
