// Vertex input of the D3D boundary (vertex_layout.h).
#include "gpu/buffers/vertex_layout.h"
#include "gpu/backend/pipeline_keys.h"
#include "gpu/d3d/hooks.h"
#include "gpu/d3d/draw.h"
#include "gpu/common/guest_memory.h"
#include "gpu/common/hash.h"
#include "gpu/common/records.h"
#include "gpu/host/services.h"
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
const char* kSemantics[] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT",
                            "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"};

// Xenos vertex format (vfetch dword 1) to the typed vertex format; unknown
// when not covered yet.
VertexFormat TypedVertexFormat(uint32_t format, bool integer, bool is_signed) {
  using F = VertexFormat;
  switch (format) {
    case 6: return integer ? (is_signed ? F::kR8G8B8A8Sint : F::kR8G8B8A8Uint)
                           : (is_signed ? F::kR8G8B8A8Snorm : F::kR8G8B8A8Unorm);
    // 16-bit pairs: only TEXCOORD, whose order XenosRecomp restores by g_SwappedTexcoords.
    case 25: return integer ? (is_signed ? F::kR16G16Sint : F::kR16G16Uint)
                            : (is_signed ? F::kR16G16Snorm : F::kR16G16Unorm);
    case 26: return integer ? (is_signed ? F::kR16G16B16A16Sint : F::kR16G16B16A16Uint)
                            : (is_signed ? F::kR16G16B16A16Snorm : F::kR16G16B16A16Unorm);
    case 31: return F::kR16G16Float;
    case 32: return F::kR16G16B16A16Float;
    case 36: return F::kR32Float;
    case 37: return F::kR32G32Float;
    case 38: return F::kR32G32B32A32Float;
    case 57: return F::kR32G32B32Float;
    default: return F::kUnknown;
  }
}

// NORMAL, TANGENT and BINORMAL are uint4 inputs of XenosRecomp read
// through tfetchR11G11B10. With SPEC_CONSTANT_R11G11B10_NORMAL it unpacks the
// 10_11_11 word; otherwise it takes the bits as float (asfloat). The cache is
// built with g_SpecConstants() == 0, so float formats get the UINT format of
// the same size, and 10_11_11 (format 16) is expanded on the CPU by the same
// arithmetic into a separate stream of four float bit patterns.
bool UintUsage(uint32_t usage) { return usage == 3 || usage == 6 || usage == 7; }
VertexFormat UintVertexFormat(uint32_t format) {
  switch (format) {
    case 16: case 31: case 32: return VertexFormat::kR32G32B32A32Uint;  // Expanded stream.
    case 36: return VertexFormat::kR32Uint;
    case 37: return VertexFormat::kR32G32Uint;
    case 38: return VertexFormat::kR32G32B32A32Uint;
    case 57: return VertexFormat::kR32G32B32Uint;
    default: return VertexFormat::kUnknown;
  }
}
// The vertex fetch is decoded in the shader. The shaders of the cache come
// from the repository's fork of XenosRecomp (tools/xenosrecomp, NARUTO_RECOMP):
// every vertex input is a
// uint4 of the raw words of its element, bound R32[G32[B32[A32]]]_UINT, and
// vfetchNaruto decodes it by its descriptor in the shared constants (c20..c23,
// one per element in fetch address order). The descriptor holds the format
// (bits 0..5), signed (6),
// integer (7), the DEC3N rule of 10_11_11 in NORMAL, TANGENT and BINORMAL (8)
// and the source of each output component (9..20: 0..3 a component in hardware
// order, X in the low bits of the swapped dword; 4 zero; 5 one). The typed
// formats stay for the template shaders of the START policy.
uint32_t FetchComponents(uint32_t format) {
  switch (format) {
    case 36: return 1;
    case 25: case 31: case 37: return 2;
    case 16: case 57: return 3;
    case 6: case 7: case 26: case 32: case 38: return 4;
    default: return 0;
  }
}
uint32_t FetchBytes(uint32_t format) {
  switch (format) {
    case 6: case 7: case 16: case 25: case 31: case 36: return 4;
    case 26: case 32: case 37: return 8;
    case 57: return 12;
    case 38: return 16;
    default: return 0;
  }
}
VertexFormat RawVertexFormat(uint32_t bytes) {
  return bytes == 4 ? VertexFormat::kR32Uint : bytes == 8 ? VertexFormat::kR32G32Uint
         : bytes == 12 ? VertexFormat::kR32G32B32Uint : VertexFormat::kR32G32B32A32Uint;
}
constexpr uint32_t Selectors(uint32_t x, uint32_t y, uint32_t z, uint32_t w) { return x | y << 3 | z << 6 | w << 9; }
// The components of n, then the defaults of the input assembler: y and z 0,
// w as given (1; 0 for the float bits of a uint input, whose 1 was a denormal).
uint32_t DefaultSelectors(uint32_t n, uint32_t w) {
  return Selectors(n > 0 ? 0 : 4, n > 1 ? 1 : 4, n > 2 ? 2 : 4, n > 3 ? 3 : w);
}
uint32_t FetchDescriptor(uint32_t format, bool is_signed, bool integer, bool dec3n, uint32_t selectors) {
  return format | uint32_t(is_signed) << 6 | uint32_t(integer) << 7 | uint32_t(dec3n) << 8 | selectors << 9;
}

// The shader commit (82141178) patches the vertex fetches of the bound VS by
// the declaration (+11812) and the stream strides (+12520) through 82140540:
// in place when the last use of the microcode has completed (82141038),
// otherwise on a copy loaded inline into the ring (82140E18), which in the native
// mode is the discard buffer. 82140540 reads the fetch templates kept in the
// VS object and writes 12-byte instructions only into the code it receives.
// The own draw runs it on an own copy of the microcode, so the fetch words are
// those of this draw on either path. Without declaration, or with container
// flag 0x40, the commit does not patch and neither does the own draw.
extern "C" void __imp__sub_82140540(PPCContext& __restrict, uint8_t*);
bool PatchVertexFetches(PPCContext& ctx, uint8_t* base, uint32_t dev, BoundShader& vs, std::string& record) {
  const uint32_t decl = Get32(base, dev + 11812), flags = Get32(base, vs.container);
  if (!decl || (flags & 0x40)) {
    record = fmt::format("{{\"declaration\":{},\"container_flags\":{},\"patched\":false}}", decl, flags);
    return true;
  }
  if (!inline_draw.patch || vs.code.size() * 4 > kPatchBytes) return false;
  for (size_t i = 0; i < vs.code.size(); ++i) Put32(base, inline_draw.patch + uint32_t(4 * i), vs.code[i]);
  std::array<uint8_t, sizeof(PPCContext)> saved{};
  std::memcpy(saved.data(), &ctx, sizeof(PPCContext));
  ctx.r1.u64 = (ctx.r1.u32 - 1024) & ~15u;  // Below the caller's frame.
  ctx.r3.u64 = vs.object;
  ctx.r4.u64 = inline_draw.patch;
  ctx.r5.u64 = decl;
  ctx.r6.u64 = dev + 12520;
  ctx.r7.u64 = 0;  // Binding slot: 1 only without a pixel shader.
  __imp__sub_82140540(ctx, base);
  std::memcpy(&ctx, saved.data(), sizeof(PPCContext));
  uint32_t changed = 0;
  for (size_t i = 0; i < vs.code.size(); ++i) {
    const uint32_t word = Get32(base, inline_draw.patch + uint32_t(4 * i));
    changed += word != vs.code[i];
    vs.code[i] = word;
  }
  record = fmt::format("{{\"declaration\":{},\"container_flags\":{},\"patched\":true,\"words_changed\":{}}}", decl,
                       flags, changed);
  return true;
}

struct VertexLayoutKey {
  uint32_t vs_object = 0, decl = 0, decl_id = 0;
  uint64_t vs_generation = 0;
  std::array<uint32_t, 4> strides{};
  bool operator==(const VertexLayoutKey& o) const {
    return vs_object == o.vs_object && decl == o.decl && decl_id == o.decl_id && vs_generation == o.vs_generation &&
           strides == o.strides;
  }
};
struct VertexLayoutKeyHash {
  size_t operator()(const VertexLayoutKey& k) const {
    const uint32_t words[] = {k.vs_object, k.decl, k.decl_id, uint32_t(k.vs_generation), uint32_t(k.vs_generation >> 32),
                              k.strides[0], k.strides[1], k.strides[2], k.strides[3]};
    return size_t(Fnv64(reinterpret_cast<const uint8_t*>(words), sizeof(words)));
  }
};
std::mutex vertex_layouts_mutex;
std::unordered_map<VertexLayoutKey, std::shared_ptr<const VertexLayout>, VertexLayoutKeyHash> vertex_layouts;
std::atomic<uint64_t> layouts_built{0}, layouts_reused{0}, layouts_uncached{0};
void ParseVertexElements(PPCContext& saved, uint8_t* base, uint32_t dev, const BoundShader& vs,
                         const CachedShader* cached, VertexLayout& L) {
  auto fail = [&](const char* reason, std::string detail) {
    L.ok = false;
    L.reason = reason;
    L.detail = std::move(detail);
  };
  BoundShader patched = vs;  // The fetch words of the bound declaration.
  if (!PatchVertexFetches(saved, base, dev, patched, L.patch_record)) return fail("vertex-fetch-patch-unproved", "\"detail\":null");
  L.patched_code = patched.code;
  const uint32_t shader = vs.container + Get32(base, vs.container + 0x18);
  const uint32_t field18 = Get32(base, shader + 0x18), count = Get32(base, shader + 0x1C);
  if (count > 16) return fail("vertex-elements-uncovered", fmt::format("\"count\":{}", count));
  uint32_t last_const = UINT32_MAX, last_stride = 0;
  std::vector<std::pair<uint32_t, uint32_t>> ordered;  // (address, value)
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t value = Get32(base, shader + 0x24 + 4 * (field18 + i));
    ordered.emplace_back(value & 0xFFF, value);
  }
  std::sort(ordered.begin(), ordered.end());
  // The patch also sorts the fetches of the declaration by fetch
  // constant and offset (mini fetches for consecutive ones of a stream), so
  // the fetch at an element's address may be another element's (the Story
  // Mode buildings: COLOR0 read COLOR1, COLOR1 the normal, ...); each keeps
  // its destination register. The patched fetch of an element is the one
  // with the element's destination (bits 12..18 of word 0) in the template
  // microcode, whose order the shader cache follows; a mini fetch takes the
  // constant and stride of the full fetch before it, in the patched order.
  struct PatchedFetch {
    uint32_t address = 0, w0 = 0, w1 = 0, w2 = 0, fetch = 0, stride = 0;
    bool mini = false;
  };
  std::vector<PatchedFetch> patched_fetches;
  for (const auto& [address, value] : ordered) {
    if (3 * address + 2 >= patched.code.size()) return fail("vertex-element-invalid", fmt::format("\"value\":{}", value));
    PatchedFetch p;
    p.address = address;
    p.w0 = patched.code[3 * address];
    p.w1 = patched.code[3 * address + 1];
    p.w2 = patched.code[3 * address + 2];
    p.mini = (p.w1 >> 30) & 1;
    p.fetch = p.mini ? last_const : ((p.w0 >> 20) & 0x1F) * 3 + ((p.w0 >> 25) & 3);
    p.stride = p.mini ? last_stride : p.w2 & 0xFF;
    last_const = p.fetch;
    last_stride = p.stride;
    patched_fetches.push_back(p);
  }
  const std::vector<uint32_t>& template_code = cached ? cached->code : vs.code;
  for (const auto& [address, value] : ordered) {
    const uint32_t usage = (value >> 12) & 0xF, usage_index = (value >> 16) & 0xF;
    if (usage >= 14 || 3 * address + 2 >= template_code.size())
      return fail("vertex-element-invalid", fmt::format("\"value\":{}", value));
    const uint32_t destination = (template_code[3 * address] >> 12) & 0x7F;
    const PatchedFetch* match = nullptr;
    uint32_t matches = 0;
    for (const auto& p : patched_fetches)
      if (((p.w0 >> 12) & 0x7F) == destination) { match = &p; ++matches; }
    // Two fetches into one register (vs-001789C1: COLOR0 into r1.xyz and
    // COLOR1 into r1.w) are told apart by the components each writes (dst_swiz,
    // bits 0..11 of word 1, 7 = not written), which the patch keeps.
    if (matches > 1) {
      auto written = [](uint32_t w1) {
        uint32_t mask = 0;
        for (unsigned c = 0; c < 4; ++c)
          if (((w1 >> (3 * c)) & 7) != 7) mask |= 1u << c;
        return mask;
      };
      const uint32_t mask = written(template_code[3 * address + 1]);
      match = nullptr;
      matches = 0;
      for (const auto& p : patched_fetches)
        if (((p.w0 >> 12) & 0x7F) == destination && written(p.w1) == mask) { match = &p; ++matches; }
    }
    if (matches != 1)
      return fail("vertex-fetch-match-unproved", fmt::format("\"address\":{},\"destination\":{},\"matches\":{}", address,
                                                             destination, matches));
    const uint32_t w0 = match->w0, w1 = match->w1, w2 = match->w2, fetch = match->fetch, stride = match->stride;
    const bool mini = match->mini;
    const uint32_t xenos_format = (w1 >> 16) & 0x3F;
    const bool is_signed = (w1 >> 12) & 1, integer = (w1 >> 13) & 1;
    const bool half_pairs = xenos_format == 25 || xenos_format == 26 || xenos_format == 31 || xenos_format == 32;
    const bool halves = xenos_format == 31 || xenos_format == 32;
    // signed_rf_mode 1 (bit 14) and exp_adjust (bits 24..29, format/ucode.h)
    // stay a guard for the packed and 16-bit integer formats.
    const bool plain = !((w1 >> 14) & 1) && !((w1 >> 24) & 0x3F);
    // Swizzle selector 6 (undefined) stays a guard; selector 7 (kKeep)
    // reads 0 (BLENDWEIGHT 16_16_16_16 of the Story Mode skinned meshes).
    bool swizzle_covered = true;
    for (unsigned c = 0; c < 4; ++c) swizzle_covered = swizzle_covered && ((w1 >> (3 * c)) & 7) != 6;
    // The template shaders of the START policy: the typed formats; an element
    // they would need expanded on the CPU leaves them without a layout.
    const bool float_packed = !UintUsage(usage) && xenos_format == 16 && plain;
    const bool dec3 = xenos_format == 7 && plain;
    const bool swizzled_pairs = !UintUsage(usage) && usage != 5 && (xenos_format == 25 || xenos_format == 26) && plain &&
                                swizzle_covered;
    const VertexFormat legacy =
        UintUsage(usage) ? (dec3 ? VertexFormat::kR32G32B32A32Uint : UintVertexFormat(xenos_format))
        : float_packed || swizzled_pairs || dec3 ? VertexFormat::kR32G32B32A32Float
        : half_pairs && usage != 5 ? VertexFormat::kUnknown
        : TypedVertexFormat(xenos_format, integer, is_signed);
    const bool legacy_covered = legacy != VertexFormat::kUnknown;
    const bool legacy_expanded = (xenos_format == 16 && (float_packed || UintUsage(usage))) ||
                                 (UintUsage(usage) && half_pairs) || swizzled_pairs || dec3;
    // The decode of the element.
    VertexFetch vf;
    vf.address = address;
    vf.components = FetchComponents(xenos_format);
    vf.patched_swizzle = w1 & 0xFFF;
    bool covered = vf.components != 0;
    uint32_t selectors = 0;
    bool dec3n = false;
    if (UintUsage(usage)) {  // NORMAL, TANGENT, BINORMAL.
      if (dec3) {
        selectors = Selectors(0, 1, 2, 3);
      } else if (xenos_format == 16) {  // DEC3N (XenosRecomp's tfetchR11G11B10); w 0.
        dec3n = true;
        selectors = Selectors(0, 1, 2, 3);
      } else if (halves) {  // The first half of a pair is the high one after the 8in32 swap.
        vf.by_endian = true;
        selectors = xenos_format == 32 ? Selectors(0, 1, 2, 3) : Selectors(0, 1, 4, 5);
        vf.descriptor_8in32 = xenos_format == 32 ? Selectors(1, 0, 3, 2) : Selectors(1, 0, 4, 5);
      } else if (UintVertexFormat(xenos_format) != VertexFormat::kUnknown) {  // The float bits.
        selectors = DefaultSelectors(vf.components, 4);
      } else {
        covered = false;
      }
    } else if (float_packed) {  // 10_11_11; w 1.
      selectors = Selectors(0, 1, 2, 5);
    } else if (dec3) {  // 2_10_10_10 (NORMAL of the Story Mode scene).
      selectors = Selectors(0, 1, 2, 3);
    } else if (usage == 5 && half_pairs) {  // TEXCOORD pairs: .yxwz after the 8in32 swap (tfetchTexcoord).
      vf.by_endian = true;
      vf.texcoord_index = int32_t(usage_index);
      selectors = DefaultSelectors(vf.components, 5);
      vf.descriptor_8in32 = xenos_format == 26 || xenos_format == 32 ? Selectors(1, 0, 3, 2) : Selectors(1, 0, 5, 4);
      covered = covered && legacy_covered;
    } else if (swizzled_pairs || (halves && swizzle_covered)) {
      // The patched swizzle (POSITION 16_16, BLENDWEIGHT, 16_16_FLOAT and
      // 16_16_16_16_FLOAT in COLOR of the Story Mode scene).
      vf.resolve_swizzle = true;
    } else if (!half_pairs && legacy_covered) {
      // The patched swizzle too (the D3DCOLOR elements of the Story Mode
      // buildings, 8_8_8_8 with Z, Y, X, W).
      selectors = DefaultSelectors(vf.components, 5);
      vf.resolve_swizzle = swizzle_covered;
    } else {
      covered = false;
    }
    vf.descriptor = FetchDescriptor(xenos_format, is_signed, integer, dec3n, selectors);
    vf.descriptor_8in32 = vf.by_endian ? FetchDescriptor(xenos_format, is_signed, integer, dec3n, vf.descriptor_8in32)
                                       : vf.descriptor;
    const uint32_t offset = (w2 >> 8) & 0x7FFFFF, bytes = FetchBytes(xenos_format);
    const VertexFormat format = covered ? RawVertexFormat(bytes) : VertexFormat::kUnknown;
    L.elements_record += fmt::format("{}{{\"usage\":{},\"index\":{},\"address\":{},\"fetch\":{},\"xenos_format\":{},"
        "\"dxgi\":{},\"legacy_dxgi\":{},\"descriptor\":{},\"stride_words\":{},\"offset_words\":{},\"mini\":{},"
        "\"fetch_address\":{},\"words\":[{},{},{}]}}", L.elements_record.empty() ? "" : ",", usage, usage_index, address,
        fetch, xenos_format, uint32_t(format), uint32_t(legacy), vf.descriptor, stride, offset, mini, match->address,
        w0, w1, w2);
    if ((w0 & 0x1F) != 0 || fetch >= 96 || !stride || format == VertexFormat::kUnknown)
      return fail("vertex-fetch-uncovered", fmt::format("\"elements\":[{}]", L.elements_record));
    if (4 * offset + bytes > 4 * stride)
      return fail("vertex-element-beyond-stride", fmt::format("\"elements\":[{}]", L.elements_record));
    auto slot_it = std::find(L.stream_fetch.begin(), L.stream_fetch.end(), fetch);
    uint32_t slot = uint32_t(slot_it - L.stream_fetch.begin());
    if (slot_it == L.stream_fetch.end()) {
      L.stream_fetch.push_back(fetch);
      L.stream_strides.push_back(stride * 4);
    } else if (L.stream_strides[slot] != stride * 4) {
      return fail("vertex-stride-conflict", fmt::format("\"elements\":[{}]", L.elements_record));
    }
    vf.slot = slot;
    L.elements.push_back({kSemantics[usage], usage_index, format, slot, offset * 4});
    L.legacy_elements.push_back({kSemantics[usage], usage_index, legacy, slot, offset * 4});
    L.legacy_ok = L.legacy_ok && legacy_covered && !legacy_expanded;
    L.fetch.push_back(vf);
  }
}
// The cached DXIL comes from the template microcode of the SDB, whose fetch
// keeps the identity destination swizzle (0x688); the runtime patch 82140540
// writes the swizzle of the declaration, (Y, X, 0, 1) for a pair. The decode
// feeds the input components so that the template fetch (its destination
// swizzle t) yields what the patched fetch (swizzle p) writes: output i reads
// input t[i], so input t[i] takes p[i] (identity template: the patched swizzle
// itself). Any template, as the (X, Y, Z, keep) template under the (Y, X,
// W, keep) patch of the Story Mode skinned meshes. A template constant or keep
// must match the patch; two outputs reading one input with different
// selectors, or selector 6, stay a guard. Components the format lacks, and
// keep, read 0, as in the interpreter.
void ResolveTemplateSwizzles(VertexLayout& L, const CachedShader& cached) {
  for (auto& vf : L.fetch) {
    if (!vf.resolve_swizzle) continue;
    const uint32_t template_swizzle = 3 * vf.address + 1 < cached.code.size()
                                          ? cached.code[3 * vf.address + 1] & 0xFFF : UINT32_MAX;
    std::array<uint32_t, 4> input{4, 4, 4, 4};  // Unread inputs: 0.
    std::array<bool, 4> set{};
    bool covered = template_swizzle != UINT32_MAX;
    for (unsigned i = 0; covered && i < 4; ++i) {
      const uint32_t ts = (template_swizzle >> (3 * i)) & 7, ps = (vf.patched_swizzle >> (3 * i)) & 7;
      if (ts == 6) covered = false;
      else if (ts >= 4) covered = ts == ps;
      else if (set[ts]) covered = input[ts] == ps;
      else { input[ts] = ps; set[ts] = true; }
    }
    if (!covered) {
      L.ok = false;
      L.reason = "vertex-template-swizzle-uncovered";
      L.detail = fmt::format("\"elements\":[{}],\"template_swizzle\":{}", L.elements_record, template_swizzle);
      return;
    }
    std::array<uint32_t, 4> sources{};
    for (unsigned i = 0; i < 4; ++i)
      sources[i] = input[i] < 4 ? (input[i] < vf.components ? input[i] : 4) : input[i] == 5 ? 5 : 4;
    const uint32_t selectors = Selectors(sources[0], sources[1], sources[2], sources[3]);
    vf.descriptor = (vf.descriptor & 0x1FF) | selectors << 9;
    vf.descriptor_8in32 = vf.descriptor;
  }
}
// The layout of the bound pair: reused, or built (parse, the tolerant
// identification of a VS the exact one missed, the template swizzles) and
// kept when the pair has a stable key. vs_cached, candidates and differing
// are updated by the tolerant identification.
std::shared_ptr<const VertexLayout> FindOrBuildVertexLayout(PPCContext& saved, uint8_t* base, uint32_t dev,
                                                            ShaderEntry* entry, const BoundShader& vs, bool quad,
                                                            CachedShader*& vs_cached, uint32_t& candidates,
                                                            uint32_t& differing) {
  VertexLayoutKey key;
  key.vs_object = vs.object;
  key.vs_generation = entry ? entry->generation : 0;
  key.decl = Get32(base, dev + 11812);
  key.decl_id = key.decl ? Get32(base, key.decl + 48) : 0;
  for (unsigned i = 0; i < 4; ++i) key.strides[i] = Get32(base, dev + 12520 + 4 * i);
  const bool no_patch = !key.decl || (Get32(base, vs.container) & 0x40);
  const bool cacheable = entry && (no_patch || key.decl_id) && !quad;
  // Each thread keeps its last layouts by key in front of the map: the map
  // never drops one and the key carries the VS generation, so a kept layout
  // stays the map's (no lock, no hash of the whole key on a hit).
  struct Kept {
    VertexLayoutKey key;
    std::shared_ptr<const VertexLayout> layout;
  };
  thread_local std::array<Kept, 64> kept;
  Kept& slot = kept[((key.vs_object ^ key.decl * 31u ^ key.strides[0]) * 0x9E3779B1u) >> 26];
  if (cacheable) {
    if (slot.layout && slot.key == key) { ++layouts_reused; return slot.layout; }
    std::lock_guard lock(vertex_layouts_mutex);
    auto it = vertex_layouts.find(key);
    if (it != vertex_layouts.end()) {
      ++layouts_reused;
      slot = {key, it->second};
      return it->second;
    }
  }
  auto layout = std::make_shared<VertexLayout>();
  // The identification of an object without its entry runs before the parse.
  // It is by hash, so a VS whose fetch words are patched is not identified
  // (identified_tolerant stays 0).
  if (!vs_cached && vs.object) {
    vs_cached = IdentifyShader(vs, true, candidates, differing);
    if (vs_cached) ++shader_identified_tolerant;
    if (entry) { entry->cached = vs_cached; entry->candidates = candidates; entry->differing = differing; }
  }
  ParseVertexElements(saved, base, dev, vs, vs_cached, *layout);
  if (layout->ok && vs_cached) ResolveTemplateSwizzles(*layout, *vs_cached);
  // The request shares the elements and carries their id.
  layout->shared_elements = std::make_shared<const std::vector<VertexElement>>(layout->elements);
  layout->shared_legacy_elements = std::make_shared<const std::vector<VertexElement>>(layout->legacy_elements);
  layout->elements_id = ElementsId(layout->elements);
  layout->legacy_elements_id = ElementsId(layout->legacy_elements);
  if (cacheable && vs_cached) {
    std::lock_guard lock(vertex_layouts_mutex);
    vertex_layouts[key] = layout;
    ++layouts_built;
    slot = {key, layout};
  } else {
    ++layouts_uncached;
  }
  return layout;
}
size_t VertexLayoutCount() {
  std::lock_guard lock(vertex_layouts_mutex);
  return vertex_layouts.size();
}
}  // namespace gpu
