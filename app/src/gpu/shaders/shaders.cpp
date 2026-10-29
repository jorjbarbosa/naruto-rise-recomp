// Shaders of the D3D boundary (shaders.h). The objects are registered at
// their birth (820F5150, 820F5348, at the end of this file) and identified once.
#include "gpu/shaders/shaders.h"
#include "gpu/diagnostics/diagnostics.h"
#include "gpu/buffers/vertex_layout.h"
#include "gpu/d3d/hooks.h"
#include "gpu/common/guest_memory.h"
#include "gpu/common/hash.h"
#include "gpu/common/records.h"
#include "gpu/common/sha256.h"
#include "gpu/host/services.h"
#include "gpu/diagnostics/profile.h"
#include "gpu/settings.h"
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
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>
#include <rex/system/xmemory.h>

using namespace gpu;

namespace gpu {
// Guest readers of the draw layer: shader objects with their container and
// physical microcode. Bindings: pixel shader at device+12684, vertex shader at
// +12688. Shader object: container at +0x368 (VS) or +0x28 (PS), metadata
// offset at +0x380 / +0x40, code base at +0x20 / +0x18.
std::string ShaderRecord(uint8_t* base, uint32_t object, bool vertex) {
  if (!object) return "null";
  const uint32_t container = object + (vertex ? 0x368 : 0x28);
  const uint32_t flags = Get32(base, container), virtual_size = Get32(base, container + 4);
  const uint32_t physical_size = Get32(base, container + 8);
  const uint32_t relative = Get32(base, object + (vertex ? 0x380 : 0x40));
  const uint32_t code_base = Get32(base, object + (vertex ? 0x20 : 0x18));
  std::string microcode;
  uint32_t code_offset = 0, code_size = 0;
  if (virtual_size <= 0x8000 && relative + 8 <= virtual_size) {
    code_offset = Get32(base, container + relative);
    code_size = Get32(base, container + relative + 4);
    microcode = PhysicalHex(base, GuestPhysicalAlias(code_base + code_offset), code_size);
  }
  return fmt::format("{{\"object\":{},\"flags\":{},\"virtual_size\":{},\"physical_size\":{},\"relative\":{},"
                     "\"code_base\":{},\"code_offset\":{},\"code_size\":{},\"object_hex\":\"{}\","
                     "\"virtual_hex\":\"{}\",\"microcode_hex\":\"{}\"}}", object, flags, virtual_size,
                     physical_size, relative, code_base, code_offset, code_size,
                     GuestHex(base, object, vertex ? 0x368 : 0x28),
                     virtual_size <= 0x8000 ? GuestHex(base, container, virtual_size) : "", microcode);
}

ShaderCache shader_cache;


// The texture fetch instructions (opcode 1) of a microcode: walks the control
// flow pairs at its start and the fetch slots of every exec (sequence bit 2i),
// as format/ucode.h of the SDK lays them out, calling f(word 0, word 1, word 2)
// until it returns false.
template <class F>
void ForEachTextureFetch(const std::vector<uint32_t>& code, F&& f) {
  const size_t n = code.size();
  size_t first = n / 3;  // Lowest instruction address seen, in units of 3 words.
  for (size_t k = 0; 3 * k + 2 < n && k < first; ++k) {
    const uint32_t d0 = code[3 * k], d1 = code[3 * k + 1], d2 = code[3 * k + 2];
    const std::array<std::pair<uint32_t, uint32_t>, 2> pair{{{d0, d1 & 0xFFFF}, {(d1 >> 16) | (d2 << 16), d2 >> 16}}};
    for (const auto& [a, b] : pair) {
      const uint32_t op = (b >> 12) & 0xF;  // Exec opcodes: 1 to 6, 13 and 14.
      if (!((op >= 1 && op <= 6) || op == 13 || op == 14)) continue;
      const uint32_t address = a & 0xFFF, count = (a >> 12) & 7, sequence = (a >> 16) & 0xFFF;
      if (address) first = std::min<size_t>(first, address);
      for (uint32_t i = 0; i < count; ++i) {
        const size_t at = 3 * size_t(address + i);
        if (!((sequence >> (2 * i)) & 1) || at + 2 >= n || (code[at] & 0x1F) != 1) continue;  // tfetch
        if (!f(code[at], code[at + 1], code[at + 2])) return;
      }
    }
  }
}
// The own sampler takes its filters from the fetch constant, as the
// translated DXIL (XenosRecomp) does. A texture fetch instruction can override
// them (mag, min, mip, aniso and the two volume filters; kUseFetchConst = 3,
// aniso 7). Returns the first overriding field, or null. None of the tfetch
// of the game's pixel shaders overrides.
const char* TfetchFilterOverride(const std::vector<uint32_t>& code) {
  const char* field = nullptr;
  ForEachTextureFetch(code, [&](uint32_t, uint32_t w, uint32_t) {
    field = ((w >> 12) & 3) != 3 ? "mag_filter" : ((w >> 14) & 3) != 3 ? "min_filter"
          : ((w >> 16) & 3) != 3 ? "mip_filter" : ((w >> 18) & 7) != 7 ? "aniso_filter"
          : ((w >> 24) & 3) != 3 ? "vol_mag_filter" : ((w >> 26) & 3) != 3 ? "vol_min_filter" : nullptr;
    return field == nullptr;
  });
  return field;
}
// The fetch constants of the texture fetches (word 0 bits 20..24) with the
// fetch dimension (word 2 bits 14..15: 0 1D, 1 2D, 2 3D or stacked, 3 cube),
// distinct. A vertex shader samples constants 16 to 19.
std::vector<std::pair<uint32_t, uint32_t>> TfetchConstants(const std::vector<uint32_t>& code) {
  std::vector<std::pair<uint32_t, uint32_t>> out;
  ForEachTextureFetch(code, [&](uint32_t w0, uint32_t, uint32_t w2) {
    const std::pair<uint32_t, uint32_t> fetch{(w0 >> 20) & 0x1F, (w2 >> 14) & 3};
    if (std::find(out.begin(), out.end(), fetch) == out.end()) out.push_back(fetch);
    return true;
  });
  return out;
}

// The package of the session's format (package.h) and its index: the line
// of the format's binaries, the contract of the SPIR-V package, and one line
// per shader (stage, name, hash of the virtual part then the microcode,
// virtual size, textures, spec mask and, in SPIR-V, the SHA-256 of the module).
void LoadShaderCache(ShaderFormat format) {
  // REX_NATIVE_SHADER_INDEX is not read; a run that sets it only records that.
  const auto started = std::chrono::steady_clock::now();
  shader_cache.format = format;
  shader_cache.package = EmbeddedShaderPackage(format, shader_cache.error);
  if (!shader_cache.package) {
    // Every draw is skipped from here on; the census only says so on close.
    REXLOG_ERROR("[narutorise] Native shader package unavailable: {}", shader_cache.error);
    return;
  }
  const ShaderPackage& package = *shader_cache.package;
  const bool spirv = format == ShaderFormat::kSpirv;
  shader_cache.record = fmt::format("\"embedded\":true,\"container_bytes\":{},\"payload_bytes\":{},"
                                    "\"payload_sha256\":\"{}\",\"{}\":{},\"variants\":{}", package.container_bytes,
                                    package.payload.size(), package.payload_sha256, spirv ? "spirv" : "dxil",
                                    package.modules.size(), package.variants.size());
  if (const char* path = std::getenv("REX_NATIVE_SHADER_INDEX"); path && *path)
    shader_cache.record += ",\"index_variable_ignored\":true";
  const std::string& text = package.index;
  size_t at = 0;
  bool format4 = false, modules = false;  // Index format 4 (identification only by hash).
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    const size_t next = end + 1;
    if (end > at && text[end - 1] == '\r') --end;  // The index is written with CRLF on Windows.
    std::vector<std::string> f;
    for (size_t s = at; s <= end;) {
      size_t t = text.find('\t', s);
      if (t == std::string::npos || t > end) t = end;
      f.emplace_back(text.substr(s, t - s));
      s = t + 1;
    }
    at = next;
    if (f.size() == 2 && f[0] == "format") { format4 = f[1] == "4"; continue; }
    if (f.size() == 2 && f[0] == (spirv ? "spirv" : "dxil")) { modules = true; continue; }
    if (f.size() == 2 && f[0] == "contract") { shader_cache.contract = f[1]; continue; }
    if (!format4 || f.size() != (spirv ? 7u : 6u) || (f[0] != "vs" && f[0] != "ps")) continue;
    CachedShader shader;
    shader.name = f[1];
    shader.virtual_size = uint32_t(std::stoul(f[3]));
    shader.textures = f[4] == "1";
    shader.spec_mask = uint32_t(std::stoul(f[5]));
    if (spirv) shader.spirv_sha256 = f[6];
    if (shader_cache.by_hash[f[0] == "vs"].emplace(std::stoull(f[2], nullptr, 16), std::move(shader)).second)
      ++shader_cache.entries;
  }
  shader_cache.loaded = modules && shader_cache.entries;
  if (!shader_cache.loaded) {
    shader_cache.error = format4 ? "index-empty" : "index-format";
    REXLOG_ERROR("[narutorise] Native shader package unavailable: {}", shader_cache.error);
  }
  if (spirv) shader_cache.record += fmt::format(",\"contract\":\"{}\"", shader_cache.contract);
  for (auto& shaders : shader_cache.by_hash)
    for (auto& [hash, shader] : shaders) shader_cache.by_name.emplace(shader.name, &shader);
  host::Record("native-d3d-shader-cache", fmt::format(
      "{},\"entries\":{},\"loaded\":{},\"load_ms\":{}", shader_cache.record, shader_cache.entries,
      shader_cache.loaded, std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started).count()));
}

std::shared_ptr<const std::vector<uint8_t>> LoadModule(CachedShader& shader) {
  std::lock_guard lock(shader_cache.mutex);
  if (shader.module) return shader.module;
  auto it = shader_cache.package->modules.find(shader.name);  // The embedded cache.
  if (it == shader_cache.package->modules.end()) return nullptr;
  auto module = DecodeShaderModule(*shader_cache.package, it->second);
  if (module.empty()) return nullptr;
  // The decoded SPIR-V is the module spirv-val checked in the build.
  if (shader_cache.format == ShaderFormat::kSpirv && Sha256Hex(module.data(), module.size()) != shader.spirv_sha256)
    return nullptr;
  shader.module = std::make_shared<const std::vector<uint8_t>>(std::move(module));
  return shader.module;
}

// The module of a cached shader for the spec constants of the draw. In
// DXIL, value 0 takes the DXIL compiled with the stub and another value inside
// the mask the variant the build linked, copied from the package once. A
// SPIR-V module serves every value.
std::shared_ptr<const std::vector<uint8_t>> LoadShader(CachedShader& shader, uint32_t spec, std::string& error) {
  spec &= shader.spec_mask;
  if (!spec || shader_cache.format == ShaderFormat::kSpirv) {
    auto module = LoadModule(shader);
    if (!module) error = "module-unreadable";
    return module;
  }
  std::lock_guard lock(shader_cache.mutex);
  if (auto it = shader.linked.find(spec); it != shader.linked.end()) return it->second;
  auto found = shader_cache.package->variants.find(fmt::format("{}+spec{}", shader.name, spec));  // The embedded cache.
  if (found == shader_cache.package->variants.end()) {
    error = "variant-missing";
    return nullptr;
  }
  ++shader_cache.variants_loaded;
  profile::Count(profile::kShaderVariants, 1);
  auto variant = std::make_shared<const std::vector<uint8_t>>(found->second.begin(), found->second.end());
  return shader.linked.emplace(spec, std::move(variant)).first->second;
}

uint32_t TransparencySpec(uint32_t spec, uint32_t spec_mask, uint32_t samples, uint32_t blend_control) {
  // RB_BLENDCONTROL color: source factor bits 0..4, operation 5..7, destination 8..12.
  const bool opaque_color = (blend_control & 0x1FFF) == 0x0001;  // ONE, ADD, ZERO
  if (!CurrentSettings().transparency_antialiasing || samples < 2 || !opaque_color || !(spec & kSpecAlphaTest) ||
      !(spec_mask & kSpecAlphaToCoverage)) return spec;
  return (spec & ~kSpecAlphaTest) | kSpecAlphaToCoverage;
}


// *why names the failed check (null object, container sizes, microcode
// size or its physical read) for the shader-object-unreadable guard.
bool ReadBoundShader(uint8_t* base, uint32_t object, bool vertex, BoundShader& s, const char** why) {
  auto fail = [why](const char* reason) {
    if (why) *why = reason;
    return false;
  };
  if (!object) return fail("null-object");
  s.object = object;
  s.container = object + (vertex ? 0x368 : 0x28);
  s.virtual_size = Get32(base, s.container + 4);
  const uint32_t relative = Get32(base, object + (vertex ? 0x380 : 0x40));
  const uint32_t code_base = Get32(base, object + (vertex ? 0x20 : 0x18));
  if (s.virtual_size < 36 || s.virtual_size > 0x8000 || relative + 8 > s.virtual_size) return fail("container-size");
  s.code_physical = GuestPhysicalAlias(code_base + Get32(base, s.container + relative));
  s.code_size = Get32(base, s.container + relative + 4);
  if (!s.code_size || s.code_size > 0x10000 || (s.code_size & 3)) return fail("microcode-size");
  std::vector<uint8_t> raw(s.code_size);
  if (!ReadPhysical(base, s.code_physical, s.code_size, raw.data())) return fail("microcode-unreadable");
  for (uint32_t i = 0; i < s.code_size; i += 4) s.code.push_back(std::byteswap(*reinterpret_cast<uint32_t*>(&raw[i])));
  s.virtual_hash = Fnv64(base + s.container, s.virtual_size);
  s.hash = Fnv64(raw.data(), raw.size(), s.virtual_hash);
  return true;
}

// By the hash of the virtual part and the microcode; the first identified
// object gives the entry its microcode (the same words, by the hash) and what
// derives from it.
CachedShader* IdentifyShader(const BoundShader& s, bool vertex, uint32_t& candidates, uint32_t& differing) {
  candidates = 0;
  differing = 0;
  auto it = shader_cache.by_hash[vertex].find(s.hash);
  if (it == shader_cache.by_hash[vertex].end() || it->second.virtual_size != s.virtual_size) return nullptr;
  candidates = 1;
  auto& c = it->second;
  std::lock_guard lock(shader_cache.mutex);
  if (!c.code_known) {
    c.code = s.code;
    c.tfetch_override = TfetchFilterOverride(c.code);
    c.fetch_constants = TfetchConstants(c.code);
    c.code_known = true;
  }
  return &c;
}

bool ReadSamplers(uint8_t* base, const BoundShader& s, std::vector<ShaderSampler>& out) {
  const uint32_t offset = Get32(base, s.container + 0x10), limit = s.container + s.virtual_size;
  if (offset < 36 || offset + 32 > s.virtual_size) return false;
  const uint32_t table = s.container + offset + 4;
  const uint32_t count = Get32(base, table + 12), info = Get32(base, table + 16);
  if (count > 1024 || uint64_t(table) + info + 20ull * count > limit) return false;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t c = table + info + 20 * i;
    if (Get16(base, c + 4) != 3) continue;  // D3DXRS_SAMPLER
    const uint64_t type_info = uint64_t(table) + Get32(base, c + 12), name = uint64_t(table) + Get32(base, c);
    if (type_info + 4 > limit || name >= limit) return false;
    ShaderSampler sampler{Get16(base, c + 6), Get16(base, uint32_t(type_info) + 2), {}};
    for (uint32_t at = uint32_t(name); at < limit && base[at] && sampler.name.size() < 64; ++at)
      sampler.name += char(base[at]);
    out.push_back(std::move(sampler));
  }
  return true;
}

std::mutex shader_entries_mutex;
std::unordered_map<uint32_t, std::shared_ptr<ShaderEntry>> shader_entries;  // object address -> entry
uint64_t shader_generations = 0;  // Under shader_entries_mutex.
// Raised under shader_entries_mutex at every change of shader_entries: the
// lookups of FindShaderEntry kept by each thread hold while it does not move.
std::atomic<uint64_t> shader_entries_epoch{0};
std::atomic<uint64_t> shader_births{0}, shader_births_unreadable{0}, shader_entries_replaced{0},
    shader_entries_forgotten{0}, shader_binds_missed{0}, shader_binds_stale{0}, shader_identified_exact{0},
    shader_identified_tolerant{0}, shader_identified_missed{0};
std::atomic<uint32_t> shader_birth_records{0};
void RegisterShaderObject(uint8_t* base, uint32_t object, bool vertex, uint32_t lr) {
  if (!object) return;
  auto entry = std::make_shared<ShaderEntry>();
  entry->vertex = vertex;
  entry->lr = lr;
  entry->code_base = Get32(base, object + (vertex ? 0x20 : 0x18));
  const char* why = "";
  const bool read = ReadBoundShader(base, object, vertex, entry->bound, &why);
  ++shader_births;
  if (!read) ++shader_births_unreadable;
  bool replaced = false;
  if (read) {
    std::lock_guard lock(shader_entries_mutex);
    entry->generation = ++shader_generations;
    replaced = shader_entries.count(object) != 0;
    shader_entries[object] = entry;
    shader_entries_epoch.fetch_add(1, std::memory_order_release);
  }
  if (replaced) ++shader_entries_replaced;
  if (gpu::Active() && Diagnostics() && shader_birth_records.fetch_add(1) < 16)
    host::Record("native-d3d-shader-birth", fmt::format(
        "\"object\":{},\"vertex\":{},\"read\":{},\"why\":\"{}\",\"virtual_hash\":\"{:016x}\",\"virtual_size\":{},"
        "\"code_size\":{},\"code_base\":{},\"generation\":{},\"replaced\":{},\"lr\":{},\"guest_thread\":{}", object,
        vertex, read, why, entry->bound.virtual_hash, entry->bound.virtual_size, entry->bound.code_size,
        entry->code_base, entry->generation, replaced, lr, GuestThread()));
}
// The entry of a bound object, or null (missed or stale), counted. Each
// thread keeps its last lookups by object (the draw runs two per capture), valid
// while the epoch of the map holds; a miss takes the lock.
std::shared_ptr<ShaderEntry> FindShaderEntry(uint8_t* base, uint32_t object, bool vertex) {
  if (!object) return nullptr;
  struct Lookup {
    uint32_t object = 0;
    uint64_t epoch = ~0ull;
    std::shared_ptr<ShaderEntry> entry;
  };
  thread_local std::array<Lookup, 64> lookups;
  Lookup& lookup = lookups[(object * 0x9E3779B1u) >> 26];
  const uint64_t epoch = shader_entries_epoch.load(std::memory_order_acquire);
  if (lookup.object != object || lookup.epoch != epoch) {
    std::lock_guard lock(shader_entries_mutex);
    auto it = shader_entries.find(object);
    lookup = {object, epoch, it != shader_entries.end() ? it->second : nullptr};
  }
  std::shared_ptr<ShaderEntry> entry = lookup.entry;
  if (!entry || entry->vertex != vertex) { ++shader_binds_missed; return nullptr; }
  if (Get32(base, object + (vertex ? 0x20 : 0x18)) != entry->code_base) { ++shader_binds_stale; return nullptr; }
  return entry;
}
// The cache name of a registered shader object, or empty: its identification,
// else the exact lookup of its hash when no draw identified it yet.
std::string ShaderNameOf(uint32_t object, bool vertex) {
  std::shared_ptr<ShaderEntry> entry;
  {
    std::lock_guard lock(shader_entries_mutex);
    auto it = shader_entries.find(object);
    if (it != shader_entries.end()) entry = it->second;
  }
  if (!entry || entry->vertex != vertex) return {};
  if (entry->identified) return entry->cached ? entry->cached->name : std::string();
  uint32_t candidates = 0, differing = 0;
  const CachedShader* cached = IdentifyShader(entry->bound, vertex, candidates, differing);
  return cached ? cached->name : std::string();
}
bool ForgetShaderObject(uint32_t object) {
  std::lock_guard lock(shader_entries_mutex);
  const bool forgotten = shader_entries.erase(object) != 0;
  if (forgotten) {
    ++shader_entries_forgotten;
    shader_entries_epoch.fetch_add(1, std::memory_order_release);
  }
  return forgotten;
}
// Exact identification of an entry, once (render thread).
void IdentifyEntry(ShaderEntry& e) {
  if (e.identified) return;
  e.identified = true;
  e.cached = IdentifyShader(e.bound, e.vertex, e.candidates, e.differing);
  e.exact = e.cached != nullptr;
  ++(e.cached ? shader_identified_exact : shader_identified_missed);
}
std::string ShaderRegistrySummary() {
  size_t entries = 0, layouts = 0;
  { std::lock_guard lock(shader_entries_mutex); entries = shader_entries.size(); }
  layouts = VertexLayoutCount();
  return fmt::format(
      "\"births\":{},\"births_unreadable\":{},\"entries\":{},\"replaced\":{},\"forgotten\":{},\"binds_missed\":{},"
      "\"binds_stale\":{},\"identified_exact\":{},\"identified_tolerant\":{},\"identified_missed\":{},"
      "\"layouts\":{},\"layouts_built\":{},\"layouts_reused\":{},\"layouts_uncached\":{}", shader_births.load(),
      shader_births_unreadable.load(), entries, shader_entries_replaced.load(), shader_entries_forgotten.load(),
      shader_binds_missed.load(), shader_binds_stale.load(), shader_identified_exact.load(),
      shader_identified_tolerant.load(), shader_identified_missed.load(), layouts, layouts_built.load(),
      layouts_reused.load(), layouts_uncached.load());
}
}  // namespace gpu
// The birth of every D3D shader object of the Jade engine (DX9ShaderDatabase):
// 820F5150 builds a vertex shader object (wrapper r3, SDB entry r4, constant
// table r5) and stores it at wrapper+4; 820F5348 does the same for a pixel
// shader. The object is registered with its unpatched microcode, before the
// native mode is active too (the SDB is loaded with the device).
extern "C" void __imp__sub_820F5150(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_820F5348(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_820F5150) {
  const uint32_t wrapper = ctx.r3.u32, lr = ctx.lr;
  __imp__sub_820F5150(ctx, base);
  gpu::profile::Timer hook_time(gpu::profile::kHookShader);
  if (wrapper) RegisterShaderObject(base, Get32(base, wrapper + 4), true, lr);
}
REX_HOOK_RAW(sub_820F5348) {
  const uint32_t wrapper = ctx.r3.u32, lr = ctx.lr;
  __imp__sub_820F5348(ctx, base);
  gpu::profile::Timer hook_time(gpu::profile::kHookShader);
  if (wrapper) RegisterShaderObject(base, Get32(base, wrapper + 4), false, lr);
}
