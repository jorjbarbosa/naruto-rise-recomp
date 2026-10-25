// Evidence of the START lot (start_evidence.h).
#include "gpu/diagnostics/start_evidence.h"
#include "gpu/diagnostics/diagnostics.h"
#include "gpu/d3d/hooks.h"
#include "gpu/d3d/device.h"
#include "gpu/d3d/resources.h"
#include "gpu/d3d/draw.h"
#include "gpu/d3d/frame.h"
#include "gpu/diagnostics/measure.h"
#include "gpu/d3d/state.h"
#include "gpu/textures/textures.h"
#include "gpu/common/guest_memory.h"
#include "gpu/common/hash.h"
#include "gpu/common/records.h"
#include "gpu/backend/dispatcher.h"
#include "gpu/host/services.h"
#include "gpu/common/sha256.h"
#include "gpu/settings.h"
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
// The START lot of the accepted contract (contratos/Start-estado-efetivo.md),
// identified in the normal flow by its data: five contiguous own draws of
// DrawIndexedVertices tail-called by the producer (LR 82108330), each with the
// signature of the lot (geometry, layout, fetch0, atlas, state, target, c38 and
// c39), whose c40 are four equal reds (R in (0, 1], G = B = 0, A = 1) followed by
// white (1, 1, 1, 1). Draws with the shaders of the lot form the census; any
// other own draw breaks the run. The values of the reference snapshot
// (geometry FNV and c40 words) are compared and recorded, not
// required. After the fifth draw of the occurrence chosen by
// StartStopOccurrence the hand-off evidence is recorded and the render thread
// stops in order; the following draw is not executed.
constexpr uint64_t kStartVerticesFnv = 0xB6FABDA832AF62CDull, kStartIndicesFnv = 0xD69FEDAA32BB0125ull;
struct StartDraw {
  uint64_t n = 0, swap = 0, producer_thread_call = 0;
  uint32_t thread = 0;
  std::array<uint32_t, 4> c40{};
  std::vector<uint8_t> vertices, indices, vs_constants, ps_constants;  // Guest bytes, big endian.
  std::string record;  // JSON fields.
  // The before.json fields, and where the bytes were read, for the
  // retention check at the fifth draw.
  std::string before, route;
  uint32_t vertex_address = 0, index_physical = 0, atlas_physical = 0;
  uint64_t atlas_fnv = 0;
  // The own request as executed (for the isolated target), its identity,
  // its copies with the fence, and the payload, variation and policy records.
  std::shared_ptr<const DrawRequest> request;
  uint64_t fence = 0;
  std::string request_sha, copies, n5, variation = "null", policy = "null";
};
template <size_t N>
bool StartRead(uint32_t address, std::array<uint32_t, N>& out) {
  std::array<uint8_t, 4 * N> bytes{};
  auto* ts = rex::runtime::ThreadState::Get();
  if (!address || !ts || !ReadVirtual(ts->memory(), address, bytes.data(), bytes.size())) return false;
  for (size_t i = 0; i < N; ++i) out[i] = BigEndianWord(bytes.data() + 4 * i);
  return true;
}
bool StartRead(uint32_t address, uint32_t& out) {
  std::array<uint32_t, 1> word{};
  if (!StartRead(address, word)) return false;
  out = word[0];
  return true;
}
bool ReadStartMetadata(StartMetadata& m) {
  if (!StartRead(0x82C0F008, m.device) || !StartRead(m.device + 0x10, m.state)) return false;
  const uint32_t s = m.state;
  if (!StartRead(s, m.dirty_words) || !StartRead(s + 0x780, m.matrix_c0_c3) || !StartRead(s + 0x9E0, m.colors_c38_c40) ||
      !StartRead(s + 0x480, m.fetch_words) || !StartRead(s + 0x2780, m.render_cache_2780) ||
      !StartRead(s + 0x30F8, m.texture_descriptor) || !StartRead(m.texture_descriptor, m.texture_descriptor_words) ||
      !StartRead(s + 0x308C, m.index_descriptor) || !StartRead(m.index_descriptor, m.index_descriptor_words) ||
      !StartRead(s + 0x318C, m.shader_bindings) || !StartRead(s + 0x2E20, m.shader_dependencies_2e20) ||
      !StartRead(s + 0x2E24, m.shader_context) || !StartRead(m.shader_context + 0x18, m.shader_context_count) ||
      !StartRead(m.shader_context + 0x20, m.shader_context_words_20) || !StartRead(s + 0x30E8, m.shader_reuse_30e8) ||
      !StartRead(m.shader_bindings[1] + 0x28, m.vs_cache_words_28) || !StartRead(s + 0x2A90, m.recycle_descriptor) ||
      !StartRead(s + 0x3090, m.target_dependencies_3090) || !StartRead(s + 0x31B0, m.target_dependencies_31b0) ||
      !StartRead(s + 0x3370, m.target_mode_3370) || !StartRead(s + 0x34B0, m.shader_mode_34b0))
    return false;
  for (size_t i = 0; i < 5; ++i)
    if (m.target_dependencies_3090[i] && !StartRead(m.target_dependencies_3090[i] + 0x18, m.target_descriptor_words_18[i]))
      return false;
  // NeedsRecycleBase: the counter is read only on the path that uses it.
  const auto pair = [](uint32_t hi, uint32_t lo) { return (uint64_t(hi) << 32) | lo; };
  const auto& v = m.vs_cache_words_28;
  const auto& c = m.shader_context_words_20;
  const auto& r = m.shader_reuse_30e8;
  const uint64_t difference = ((pair(v[2], v[3]) ^ pair(r[0], r[1])) & pair(c[0], c[1])) |
                              ((pair(v[4], v[5]) ^ pair(r[2], r[3])) & pair(c[2], c[3]));
  if ((m.dirty_words[5] & 0x1E0000u) && v[6] && (v[0] != c[4] || difference)) {
    if (!StartRead(m.recycle_descriptor, m.recycle_base)) return false;
    m.recycle_base_observed = 1;
  }
  return m.read = true;
}
// vs_route and the fixed cache words (EXPECTED) of normalize-start-state.py:
// the contract accepts the cached base reused without patch (mask 1A0000,
// cached identity equal to the context's and no masked difference) or a clean
// inheritance (no shader mask); the in-place fetch patch and the regenerated
// copy are rejected.
const char* StartVsRoute(const StartMetadata& m) {
  if (!m.read) return "unread";
  const auto pair = [](uint32_t hi, uint32_t lo) { return (uint64_t(hi) << 32) | lo; };
  const uint64_t dirty = pair(m.dirty_words[4], m.dirty_words[5]);
  if (!(dirty & 0x1E0000)) return "inherited";
  const auto& v = m.vs_cache_words_28;
  const auto& c = m.shader_context_words_20;
  const auto& r = m.shader_reuse_30e8;
  const uint64_t mismatch = ((pair(v[2], v[3]) ^ pair(r[0], r[1])) & pair(c[0], c[1])) |
                            ((pair(v[4], v[5]) ^ pair(r[2], r[3])) & pair(c[2], c[3]));
  if (dirty != 0x1A0000 || m.render_cache_2780[(0x2ABC - 0x2780) / 4] != 0x2400 || m.shader_mode_34b0)
    return "unsupported";
  if (v[0] == c[4] && !mismatch) return "cached-base-fast-reuse";
  const uint32_t cursor = m.render_cache_2780[(0x2A9C - 0x2780) / 4], old = v[6];
  return old && (cursor - old) < (cursor - m.recycle_base) ? "regenerated-copy" : "in-place-fetch-patch";
}
bool StartContractCache(const StartMetadata& m) {
  static const std::pair<uint32_t, uint32_t> expected[] = {
      {0x2880, 0x14000500}, {0x2884, 0}, {0x28B8, 0}, {0x28BC, 0x20002000}, {0x28C0, 0}, {0x28C4, 0},
      {0x28C8, 0x02D00500}, {0x28DC, 0xF}, {0x2904, 0}, {0x2934, 0x00700264}, {0x2938, 0x07000706},
      {0x293C, 0xAA000004}, {0x2940, 0x10}, {0x2944, 0x80000}, {0x2948, 0x18000}, {0x294C, 0x43F}, {0x2954, 4},
      {0x29C0, 4}, {0x29C4, 0x40000000}, {0x29C8, 0x3F800000}, {0x29CC, 0x40000000}, {0x29D0, 0x3F800000}};
  if (!m.read || m.target_mode_3370 || m.dirty_words[10] || m.dirty_words[11] ||
      m.render_cache_2780[(0x2ABC - 0x2780) / 4] != 0x2400)
    return false;
  for (const auto& [offset, value] : expected)
    if (m.render_cache_2780[(offset - 0x2780) / 4] != value) return false;
  return true;
}
std::string StartMetadataJson(const StartMetadata& m) {
  if (!m.read) return "null";
  std::string targets;
  for (const auto& t : m.target_descriptor_words_18)
    targets += fmt::format("{}[{}]", targets.empty() ? "" : ",", fmt::join(t, ","));
  return fmt::format(
      "{{\"device\":{},\"state\":{},\"texture_descriptor\":{},\"dirty_words\":[{}],\"matrix_c0_c3\":[{}],"
      "\"colors_c38_c40\":[{}],\"fetch_words\":[{}],\"render_cache_2780\":[{}],\"texture_descriptor_words\":[{}],"
      "\"index_descriptor\":{},\"index_descriptor_words\":[{}],\"shader_bindings\":[{}],"
      "\"shader_dependencies_2e20\":[{}],\"shader_context\":{},\"shader_context_words_20\":[{}],"
      "\"shader_context_count\":{},\"shader_reuse_30e8\":[{}],\"vs_cache_words_28\":[{}],\"recycle_descriptor\":{},"
      "\"recycle_base\":{},\"recycle_base_observed\":{},\"target_dependencies_3090\":[{}],"
      "\"target_descriptor_words_18\":[{}],\"target_dependencies_31b0\":[{}],\"target_mode_3370\":{},"
      "\"shader_mode_34b0\":{}}}", m.device, m.state, m.texture_descriptor, fmt::join(m.dirty_words, ","),
      fmt::join(m.matrix_c0_c3, ","), fmt::join(m.colors_c38_c40, ","), fmt::join(m.fetch_words, ","),
      fmt::join(m.render_cache_2780, ","), fmt::join(m.texture_descriptor_words, ","), m.index_descriptor,
      fmt::join(m.index_descriptor_words, ","), fmt::join(m.shader_bindings, ","),
      fmt::join(m.shader_dependencies_2e20, ","), m.shader_context, fmt::join(m.shader_context_words_20, ","),
      m.shader_context_count, fmt::join(m.shader_reuse_30e8, ","), fmt::join(m.vs_cache_words_28, ","),
      m.recycle_descriptor, m.recycle_base, m.recycle_base_observed, fmt::join(m.target_dependencies_3090, ","),
      targets, fmt::join(m.target_dependencies_31b0, ","), m.target_mode_3370, m.shader_mode_34b0);
}
struct StartLot {
  std::mutex mutex;
  std::vector<StartDraw> run;  // Contiguous own draws with the signature.
  uint64_t rejected = 0, last_run_end = 0, occurrences = 0;
} start_lot;
std::atomic<uint64_t> start_census{0};
// Occurrence of the lot where the render thread stops: 1 (default) is the
// hand-off point; REX_NATIVE_START_STOP_OCCURRENCE (up to 8) lets the census
// follow the frames after it, with the earlier lots recorded in short form.
uint64_t StartStopOccurrence() {
  static const uint64_t value = [] {
    const char* text = std::getenv("REX_NATIVE_START_STOP_OCCURRENCE");
    const int n = text ? std::atoi(text) : 1;
    return uint64_t(n >= 1 && n <= 8 ? n : 1);
  }();
  return value;
}

std::string StartWords(uint8_t* base, uint32_t address, uint32_t count) {
  std::string out;
  for (uint32_t i = 0; i < count; ++i) out += fmt::format("{}{}", i ? "," : "", Get32(base, address + 4 * i));
  return out;
}
std::vector<uint8_t> StartPhysical(uint8_t* base, uint32_t physical, uint32_t size) {
  std::vector<uint8_t> out(size);
  if (!ReadPhysical(base, physical, size, out.data())) out.clear();
  return out;
}
uint64_t StartFnv(const std::vector<uint8_t>& bytes) { return bytes.empty() ? 0 : Fnv64(bytes.data(), bytes.size()); }
bool StartRed(const std::array<uint32_t, 4>& c) {
  float r, g, b;
  std::memcpy(&r, &c[0], 4);
  std::memcpy(&g, &c[1], 4);
  std::memcpy(&b, &c[2], 4);
  return std::isfinite(r) && r > 0 && r <= 1 && g == 0 && b == 0 && c[3] == kOneWord;
}
bool StartWhite(const std::array<uint32_t, 4>& c) {
  return c == std::array<uint32_t, 4>{kOneWord, kOneWord, kOneWord, kOneWord};
}
// Guest bytes of the occurrence next to the frame readbacks, for the comparator.
std::string WriteStartFile(const std::string& name, const std::vector<uint8_t>& bytes) {
  const char* dir = std::getenv("REX_NATIVE_FRAME_DIR");
  if (!dir || !*dir || bytes.empty()) return "";
  const std::string path = fmt::format("{}/start-occurrence-{}", dir, name);
  std::FILE* out = std::fopen(path.c_str(), "wb");
  const bool ok = out && std::fwrite(bytes.data(), 1, bytes.size(), out) == bytes.size();
  if (out) std::fclose(out);
  return ok ? "start-occurrence-" + name : "write-failed";
}
// Guest words holding a vertex fetch (address | 3, size and any
// endian) that ends where fetch95 of the fifth draw ends, as a vertex-buffer
// header Format (+0x18) does before SetStreamSource adds the offset. Scans,
// page by page, the guest virtual range 0x40000000-0x80000000 and the title
// image and data 0x82000000-0x84000000; the device block is excluded. A pair
// split across two pages is not seen.
std::string StartVertexHeaders(uint8_t* base, uint32_t dev, uint32_t address, uint32_t end) {
  std::string out;
  uint32_t found = 0;
  uint64_t pages = 0;
  std::vector<uint8_t> page(0x1000);
  const std::array<std::pair<uint32_t, uint32_t>, 2> ranges{{{0x40000000u, 0x80000000u}, {0x82000000u, 0x84000000u}}};
  for (const auto& [lo, hi] : ranges)
    for (uint64_t at = lo; at < hi && found < 16; at += page.size()) {
      if (!CopyFromGuest(GuestMemory(), uint32_t(at), page.data(), page.size())) continue;
      ++pages;
      for (uint32_t i = 0; i + 8 <= page.size() && found < 16; i += 4) {
        const uint32_t w0 = BigEndianWord(&page[i]), w1 = BigEndianWord(&page[i + 4]);
        if ((w0 & 3) != 3) continue;
        const uint32_t start = w0 & ~3u, bytes = ((w1 >> 2) & 0xFFFFFF) * 4, word = uint32_t(at) + i;
        if (start > address || uint64_t(start) + bytes != end || (word >= dev && word < dev + 0x5800)) continue;
        ++found;
        out += fmt::format("{}{{\"word\":{},\"start\":{},\"bytes\":{},\"context_hex\":\"{}\"}}", out.empty() ? "" : ",",
                           word, start, bytes, GuestHex(base, word - 0x18, 32));
      }
    }
  return fmt::format("{{\"end\":{},\"pages_read\":{},\"found\":{},\"capped\":{},\"candidates\":[{}]}}", end, pages,
                     found, found >= 16, out);
}
// Identity and lifetime evidence of a bound header that no hooked creator
// registered: its words, refcount (+4) and fence (+8), registry entry and the
// D750 destructions seen for its address.
std::string StartHeader(uint8_t* base, uint32_t header, uint32_t bytes) {
  if (!header) return "null";
  ResourceRegistry::Entry entry{};
  const bool registered = resources.Find(header, entry);
  return fmt::format("{{\"address\":{},\"hex\":\"{}\",\"refcount\":{},\"fence\":{},\"registered\":{},\"generation\":{},"
                     "\"destroyed\":{}}}", header, GuestHex(base, header, bytes), Get32(base, header + 4),
                     Get32(base, header + 8), registered, registered ? entry.generation : 0,
                     texture_content.Destroyed(header));
}
void RejectStartRun(std::string_view reason) {
  if (start_lot.run.empty()) return;
  start_lot.last_run_end = start_lot.run.back().n;
  if (++start_lot.rejected <= 32) {
    std::string draws;
    for (const auto& d : start_lot.run)
      draws += fmt::format("{}{{\"own_draw\":{},\"swap\":{},\"c40\":[{}]}}", draws.empty() ? "" : ",", d.n, d.swap,
                           fmt::join(d.c40, ","));
    host::Record("native-d3d-start-run-rejected", fmt::format(
        "\"reason\":\"{}\",\"rejected\":{},\"draws\":[{}]", reason, start_lot.rejected, draws));
  }
  start_lot.run.clear();
}

StartPolicy start_policy;
std::vector<uint8_t> ReadHostFile(const std::string& path) {
  std::vector<uint8_t> bytes;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return bytes;
  char chunk[65536];
  for (size_t n; (n = std::fread(chunk, 1, sizeof(chunk), file)) > 0;) bytes.insert(bytes.end(), chunk, chunk + n);
  std::fclose(file);
  return bytes;
}
void LoadStartPolicy() {
  if (!Diagnostics()) return;
  const char* dir = std::getenv("REX_NATIVE_START_POLICY");
  if (!dir || !*dir) return;
  auto& p = start_policy;
  p.requested = true;
  p.dir = dir;
  const auto text = ReadHostFile(p.dir + "/policy.tsv");
  if (text.empty()) { p.error = "policy-file-missing"; return; }
  p.file_sha = Sha256Hex(text.data(), text.size());
  std::map<std::string, std::string> fields;
  std::string_view rest(reinterpret_cast<const char*>(text.data()), text.size());
  while (!rest.empty()) {
    const size_t end = rest.find('\n');
    std::string_view line = rest.substr(0, end);
    rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    const size_t tab = line.find('\t');
    if (tab != std::string_view::npos) fields[std::string(line.substr(0, tab))] = std::string(line.substr(tab + 1));
  }
  for (const char* key : {"policy", "vs_cache", "ps_cache", "vs_ucode_sha256", "ps_ucode_sha256", "vs_dxil_sha256",
                          "ps_dxil_sha256"})
    if (fields[key].empty()) { p.error = std::string("policy-field-missing-") + key; return; }
  p.name = fields["policy"];
  p.vs_cache = fields["vs_cache"];
  p.ps_cache = fields["ps_cache"];
  p.vs_ucode = fields["vs_ucode_sha256"];
  p.ps_ucode = fields["ps_ucode_sha256"];
  p.vs_dxil_sha = fields["vs_dxil_sha256"];
  p.ps_dxil_sha = fields["ps_dxil_sha256"];
  auto load = [&](const char* file, const std::string& sha) -> std::shared_ptr<const std::vector<uint8_t>> {
    auto bytes = std::make_shared<std::vector<uint8_t>>(ReadHostFile(p.dir + "/" + file));
    if (bytes->empty() || Sha256Hex(bytes->data(), bytes->size()) != sha) return nullptr;
    return bytes;
  };
  p.vs_dxil = load("vs.dxil", p.vs_dxil_sha);
  p.ps_dxil = load("ps.dxil", p.ps_dxil_sha);
  if (!p.vs_dxil || !p.ps_dxil) { p.error = "policy-dxil-sha-mismatch"; return; }
  p.loaded = true;
}
std::string StartPolicyJson() {
  const auto& p = start_policy;
  if (!p.requested) return "null";
  return fmt::format("{{\"policy\":\"{}\",\"policy_file_sha256\":\"{}\",\"loaded\":{},\"error\":\"{}\",\"vs_cache\":\"{}\","
                     "\"ps_cache\":\"{}\",\"vs_ucode_sha256\":\"{}\",\"ps_ucode_sha256\":\"{}\",\"vs_dxil_sha256\":\"{}\","
                     "\"ps_dxil_sha256\":\"{}\",\"selections\":{}}}", p.name, p.file_sha, p.loaded, p.error, p.vs_cache,
                     p.ps_cache, p.vs_ucode, p.ps_ucode, p.vs_dxil_sha, p.ps_dxil_sha, p.selections.load());
}
std::vector<uint8_t> BigEndianBytes(const uint32_t* words, size_t count) {
  std::vector<uint8_t> out;
  out.reserve(4 * count);
  for (size_t i = 0; i < count; ++i)
    for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(words[i] >> s));
  return out;
}
// Guest (big endian) words of a little-endian host constant block. A
// request carries the whole block as a vector (4096 bytes) or nothing; the
// shadows keep the array.
std::vector<uint8_t> GuestConstantBytes(const uint8_t* host, size_t size) {
  std::vector<uint8_t> out(size);
  for (size_t i = 0; i + 4 <= size; i += 4)
    for (size_t b = 0; b < 4; ++b) out[i + b] = host[i + 3 - b];
  return out;
}
std::vector<uint8_t> GuestConstantBytes(const std::array<uint8_t, 4096>& host) { return GuestConstantBytes(host.data(), host.size()); }
std::vector<uint8_t> GuestConstantBytes(std::span<const uint8_t> host) { return GuestConstantBytes(host.data(), host.size()); }
uint32_t HostWord(const std::array<uint8_t, 4096>& host, size_t index) {
  uint32_t v = 0;
  std::memcpy(&v, &host[4 * index], 4);
  return v;
}
uint32_t HostWord(std::span<const uint8_t> host, size_t index) {
  uint32_t v = 0;
  if (4 * index + 4 <= host.size()) std::memcpy(&v, &host[4 * index], 4);
  return v;
}
std::string HexBytes(const std::vector<uint8_t>& bytes) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(2 * bytes.size());
  for (uint8_t b : bytes) {
    out += hex[b >> 4];
    out += hex[b & 15];
  }
  return out;
}

// Identity of one own request (DrawRequest): every field that reaches
// the engine except the color target, which is the only one the isolated
// execution changes. Texture levels sent with the request are included.
std::string StartRequestSha(const DrawRequest& r) {
  Sha256 h;
  auto put = [&](const void* p, size_t n) {
    const uint64_t size = n;
    h.Update(&size, 8);
    if (n) h.Update(p, n);
  };
  auto put_value = [&](const auto& v) { put(&v, sizeof(v)); };
  put(r.vs.key.data(), r.vs.key.size());
  put(r.ps.key.data(), r.ps.key.size());
  put(r.vs.bytes ? r.vs.bytes->data() : nullptr, r.vs.bytes ? r.vs.bytes->size() : 0);
  put(r.ps.bytes ? r.ps.bytes->data() : nullptr, r.ps.bytes ? r.ps.bytes->size() : 0);
  static const std::vector<VertexElement> no_elements;
  const auto& elements = r.elements ? *r.elements : no_elements;
  put_value(elements.size());
  for (const auto& e : elements) {
    put(e.semantic, std::strlen(e.semantic));
    const uint32_t fields[4] = {e.semantic_index, uint32_t(e.format), e.slot, e.offset};
    put_value(fields);
  }
  put_value(r.streams.size());
  for (const auto& s : r.streams) {
    put_value(s.stride);
    put(s.data.data(), s.data.size());
  }
  put(r.indices.data(), r.indices.size());
  const uint32_t draw_words[] = {r.index32, r.index_count, uint32_t(r.topology), uint32_t(r.base_vertex), r.depth_key,
                                 r.blend_control, r.color_mask, r.alpha_to_coverage, r.depth_enable, r.depth_write,
                                 r.depth_func, r.stencil_enable, r.stencil_front, r.stencil_back, r.stencil_ref,
                                 r.stencil_read_mask, r.stencil_write_mask, r.cull_front, r.cull_back, r.front_ccw};
  put_value(draw_words);
  put(r.vs_constants.data(), r.vs_constants.size());
  put(r.ps_constants.data(), r.ps_constants.size());
  put(r.shared_constants.data(), r.shared_constants.size());
  put_value(r.blend_factor);
  put_value(r.viewport);
  put_value(r.scissor);
  put_value(r.textures.size());
  for (const auto& t : r.textures) {
    const uint32_t words[] = {t.reg, t.key, t.resolved, uint32_t(t.format), t.width, t.height, t.levels,
                              uint32_t(t.min_filter), uint32_t(t.mag_filter), uint32_t(t.mip_filter), t.anisotropic,
                              uint32_t(t.address_u), uint32_t(t.address_v), uint32_t(t.address_w), t.max_anisotropy};
    put_value(words);
    put_value(t.component_mapping);
    put_value(t.layout);
    put_value(t.version);
    const float lods[] = {t.lod_bias, t.min_lod, t.max_lod};
    put_value(lods);
    put_value(t.data.size());
    for (const auto& level : t.data) put(level.data(), level.size());
  }
  return h.Finish();
}
// Copies of one own request and the fence of its submission (owners of the VB,
// IB, constants and textures).
std::string StartCopiesJson(const DrawRequest& r, uint64_t fence) {
  std::string streams, textures;
  for (const auto& s : r.streams)
    streams += fmt::format("{}{{\"stride\":{},\"bytes\":{},\"sha256\":\"{}\"}}", streams.empty() ? "" : ",", s.stride,
                           s.data.size(), Sha256Hex(s.data.data(), s.data.size()));
  for (const auto& t : r.textures) {
    size_t bytes = 0;
    for (const auto& level : t.data) bytes += level.size();
    textures += fmt::format("{}{{\"reg\":{},\"key\":{},\"format\":{},\"size\":[{},{}],\"levels\":{},\"layout\":{},"
                            "\"version\":{},\"levels_sent\":{},\"bytes_sent\":{}}}", textures.empty() ? "" : ",", t.reg,
                            t.key, uint32_t(t.format), t.width, t.height, t.levels, t.layout, t.version, t.data.size(),
                            bytes);
  }
  return fmt::format("{{\"fence\":{},\"vs_key\":\"{}\",\"ps_key\":\"{}\",\"vs_dxil_sha256\":\"{}\",\"ps_dxil_sha256\":\"{}\","
                     "\"streams\":[{}],\"indices\":{{\"bytes\":{},\"sha256\":\"{}\"}},\"vs_constants_sha256\":\"{}\","
                     "\"ps_constants_sha256\":\"{}\",\"shared_constants_sha256\":\"{}\",\"textures\":[{}],"
                     "\"owned_until_fence\":\"engine submission keeps the upload of these copies until the fence\"}}",
                     fence, r.vs.key, r.ps.key,
                     r.vs.bytes ? Sha256Hex(r.vs.bytes->data(), r.vs.bytes->size()) : "",
                     r.ps.bytes ? Sha256Hex(r.ps.bytes->data(), r.ps.bytes->size()) : "", streams,
                     r.indices.size(), Sha256Hex(r.indices.data(), r.indices.size()),
                     Sha256Hex(r.vs_constants.data(), r.vs_constants.size()),
                     Sha256Hex(r.ps_constants.data(), r.ps_constants.size()),
                     Sha256Hex(r.shared_constants.data(), r.shared_constants.size()), textures);
}

// The lot's draw as the entry sees it, before the original commit: tail call of
// the producer at LR 82108330, 96 indices of a triangle list, fetch0 of the
// contract, c38 = 1, c39 = 0 and c40 red or white. NoteStartDraw checks the
// whole signature after the draw.
bool StartEntryCandidate(uint8_t* base, uint32_t dev, bool indexed, uint32_t lr, uint32_t primitive,
                         uint32_t index_count) {
  if (!Diagnostics() || !indexed || lr != kStartLr || primitive != 4 || index_count != 96) return false;
  std::array<uint32_t, 6> f0{};
  for (unsigned i = 0; i < 6; ++i) f0[i] = Get32(base, dev + 0x480 + 4 * i);
  if (f0[0] != 0x08000002 || (f0[1] & 0xFFF) != 2 || f0[2] != 0x001FE3FF || f0[3] != 0x00A80248 || f0[4] != 3 ||
      f0[5] != 0x218)
    return false;
  std::array<uint32_t, 4> c40{};
  for (unsigned i = 0; i < 4; ++i) {
    if (Get32(base, dev + 0x780 + 16 * 38 + 4 * i) != kOneWord || Get32(base, dev + 0x780 + 16 * 39 + 4 * i) != 0)
      return false;
    c40[i] = Get32(base, dev + 0x780 + 16 * 40 + 4 * i);
  }
  return StartRed(c40) || StartWhite(c40);
}

// Variation B1 (the five draws of the lot),
// asked by REX_NATIVE_START_VARIATION=B1 in a diagnostic boot. In each draw of
// the run that becomes the accepted occurrence (the first draw reuses the
// cached VS base and is red, the next four are the contiguous producer draws):
//   1. state+0x78C (c0.w) must hold the A word of the frozen pair of the draw;
//   2. the B word is written into the guest state before the original commit
//      and the copy of the own request;
//   3. the c0-c3 dirty group (0x80000000 of the high word state+0) is ORed;
//   4. after the draw the exact A word is put back and only that group is
//      rescheduled, with application and restoration recorded.
// Any divergence rejects the boot. The variation is never applied only to the
// copy of the request.
constexpr std::array<std::pair<uint32_t, uint32_t>, 5> kStartVariationPairs{{
    {0xBF7E0A3D, 0xBF7AD70A}, {0xBF80051F, 0xBF7CD70B}, {0xBF7FA3D7, 0xBF7C70A4},
    {0xBF7E70A4, 0xBF7B3D71}, {0xBF7F0A3D, 0xBF7BD70A}}};
int StartVariationMode() {  // 0 none, 1 B1, -1 not recognized.
  static const int mode = [] {
    if (!Diagnostics()) return 0;
    const char* text = std::getenv("REX_NATIVE_START_VARIATION");
    if (!text || !*text) return 0;
    return std::string_view(text) == "B1" ? 1 : -1;
  }();
  return mode;
}
struct StartVariation {
  std::mutex mutex;
  size_t applied = 0;  // Draws of the run varied and restored so far.
  bool finished = false;
  uint32_t thread = 0;
  uint64_t next_thread_call = 0;
  std::vector<uint64_t> own_draws;
  std::string records;
} start_variation;
[[noreturn]] void RejectStartVariation(PPCContext& ctx, std::string_view reason, std::string_view detail) {
  host::Record("native-d3d-start-variation-rejected", fmt::format(
      "\"reason\":\"{}\",\"applied\":{},\"detail\":{{{}}},\"records\":[{}]", reason, start_variation.applied, detail,
      start_variation.records));
  StopInOrder(ctx, "native-d3d-start-variation-divergence", 88);
}
// At the entry of every own draw while B1 is asked, before the original commit.
StartVariationDraw ApplyStartVariation(PPCContext& ctx, uint8_t* base, uint32_t dev, bool candidate,
                                       const StartMetadata& pre, uint32_t thread) {
  StartVariationDraw d;
  std::string reason, detail;
  {
    std::lock_guard lock(start_variation.mutex);
    auto& v = start_variation;
    if (v.finished) return d;
    const ProducerBefore pb = producer_before;
    const bool in_run = v.applied > 0;
    if (!candidate) {
      if (!in_run) return d;
      reason = "run-broken-by-other-draw";
    } else if (!in_run) {
      std::array<uint32_t, 4> c40{};
      for (unsigned i = 0; i < 4; ++i) c40[i] = Get32(base, dev + 0x780 + 16 * 40 + 4 * i);
      if (std::string_view(StartVsRoute(pre)) != "cached-base-fast-reuse" || !StartRed(c40) || !pb.arrival ||
          pb.drawn)
        return d;
    } else if (pb.drawn || !pb.arrival || pb.thread_call != v.next_thread_call || thread != v.thread) {
      reason = "run-not-contiguous";
    }
    if (reason.empty()) {
      d.k = v.applied;
      d.a = kStartVariationPairs[d.k].first;
      d.b = kStartVariationPairs[d.k].second;
      d.observed_a = Get32(base, dev + 0x78C);
      d.dirty_before = Get32(base, dev);
      if (d.observed_a != d.a) {
        reason = "a-word-differs";
      } else {
        Put32(base, dev + 0x78C, d.b);
        Put32(base, dev, d.dirty_before | 0x80000000u);
        d.observed_b = Get32(base, dev + 0x78C);
        d.dirty_applied = Get32(base, dev);
        if (d.observed_b != d.b || d.dirty_applied != (d.dirty_before | 0x80000000u)) {
          reason = "b-word-not-applied";
        } else {
          d.applied = true;
          v.thread = thread;
          v.next_thread_call = pb.thread_call + 1;
        }
      }
    }
    detail = fmt::format("\"k\":{},\"a\":{},\"b\":{},\"observed_a\":{},\"dirty_before\":{},\"thread_call\":{},"
                         "\"thread\":{},\"candidate\":{}", d.k, d.a, d.b, d.observed_a, d.dirty_before, pb.thread_call,
                         thread, candidate);
  }
  if (!reason.empty()) RejectStartVariation(ctx, reason, detail);
  return d;
}
// After the own draw: the exact A word back, the c0-c3 group rescheduled.
// request_c0w is the c0.w word that the own request copied.
std::string RestoreStartVariation(PPCContext& ctx, uint8_t* base, uint32_t dev, const StartVariationDraw& d,
                                  uint64_t own_draw, uint32_t request_c0w) {
  if (!d.applied) return "null";
  Put32(base, dev + 0x78C, d.a);
  const uint32_t restored = Get32(base, dev + 0x78C);
  const uint32_t dirty_after = Get32(base, dev);
  Put32(base, dev, dirty_after | 0x80000000u);
  const uint32_t dirty_restored = Get32(base, dev);
  const std::string record = fmt::format(
      "{{\"k\":{},\"own_draw\":{},\"a\":{},\"b\":{},\"observed_a\":{},\"dirty_before\":{},\"dirty_applied\":{},"
      "\"observed_b\":{},\"request_c0w\":{},\"request_has_b\":{},\"restored\":{},\"dirty_after\":{},"
      "\"dirty_restored\":{},\"restored_exact\":{}}}", d.k, own_draw, d.a, d.b, d.observed_a, d.dirty_before,
      d.dirty_applied, d.observed_b, request_c0w, request_c0w == d.b, restored, dirty_after, dirty_restored,
      restored == d.a && dirty_restored == (dirty_after | 0x80000000u));
  std::string reason;
  {
    std::lock_guard lock(start_variation.mutex);
    auto& v = start_variation;
    v.records += fmt::format("{}{}", v.records.empty() ? "" : ",", record);
    v.own_draws.push_back(own_draw);
    if (request_c0w != d.b) reason = "request-without-b";
    else if (restored != d.a || dirty_restored != (dirty_after | 0x80000000u)) reason = "restore-not-exact";
    else if (++v.applied == kStartVariationPairs.size()) v.finished = true;
  }
  if (!reason.empty()) RejectStartVariation(ctx, reason, fmt::format("\"own_draw\":{}", own_draw));
  return record;
}
std::string StartVariationJson() {
  if (!StartVariationMode()) return "null";
  std::lock_guard lock(start_variation.mutex);
  return fmt::format("{{\"name\":\"B1\",\"declaration\":\"stage2-3d/variation-declaration.json\",\"applied\":{},"
                     "\"finished\":{},\"own_draws\":[{}],\"records\":[{}]}}", start_variation.applied,
                     start_variation.finished, fmt::join(start_variation.own_draws, ","), start_variation.records);
}

bool ReadStartShaderPayload(uint8_t* base, uint32_t binding, bool vertex, StartShaderPayload& p) {
  p = {};
  p.binding = binding;
  if (!binding || binding > UINT32_MAX - (vertex ? 0x387u : 0x47u)) return false;
  p.relative_metadata = Get32(base, binding + (vertex ? 0x380 : 0x40));
  p.code_base = Get32(base, binding + (vertex ? 0x20 : 0x18));
  const uint32_t container = binding + (vertex ? 0x368 : 0x28);
  for (unsigned i = 0; i < 8; ++i) p.container_header[i] = Get32(base, container + 4 * i);
  if (p.container_header[0] != (vertex ? 0x102A1101u : 0x102A1100u) || p.relative_metadata > 0x4000 ||
      uint64_t(p.relative_metadata) + 32 > p.container_header[1])
    return false;
  p.metadata_address = container + p.relative_metadata;
  for (unsigned i = 0; i < 8; ++i) p.metadata[i] = Get32(base, p.metadata_address + 4 * i);
  const uint32_t size = p.metadata[1];
  if (!size || size > 65536 || (size & 3) || uint64_t(p.metadata[0]) + size > p.container_header[2] ||
      uint64_t(p.code_base) + p.metadata[0] > UINT32_MAX)
    return false;
  p.physical = GuestPhysicalAlias(p.code_base + p.metadata[0]);
  p.bytes = StartPhysical(base, p.physical, size);
  return p.read = p.bytes.size() == size;
}
std::string StartShaderPayloadJson(const StartShaderPayload& p, bool vertex) {
  return fmt::format("{{\"stage\":\"{}\",\"read\":{},\"binding\":{},\"metadata_address\":{},\"relative_metadata\":{},"
                     "\"code_base\":{},\"physical\":{},\"metadata_words\":[{}],\"container_header_words\":[{}],"
                     "\"bytes_hex\":\"{}\"}}", vertex ? "vs" : "ps", p.read, p.binding, p.metadata_address,
                     p.relative_metadata, p.code_base, p.physical, fmt::join(p.metadata, ","),
                     fmt::join(p.container_header, ","), HexBytes(p.bytes));
}
void ReadStartEntry(uint8_t* base, const StartMetadata& before, StartLiveInputs& live) {
  if (!before.read) return;
  live.atlas_physical = before.fetch_words[1] & 0xFFFFF000u;
  const auto first = StartPhysical(base, live.atlas_physical, 1024 * 256);
  live.atlas_read = first.size() == 1024 * 256;
  live.atlas_sha = live.atlas_read ? Sha256Hex(first.data(), first.size()) : "";
  for (size_t i = 0; i < 2; ++i) ReadStartShaderPayload(base, before.shader_bindings[i], i == 1, live.shaders[i]);
  for (size_t i = 0; i < 2; ++i) ReadStartShaderPayload(base, before.shader_bindings[i], i == 1, live.shaders_repeat[i]);
  const auto second = StartPhysical(base, live.atlas_physical, 1024 * 256);
  live.atlas_repeat_equal = live.atlas_read && second == first;
  ReadStartMetadata(live.repeat);
}
void ReadStartAfter(uint8_t* base, StartLiveInputs& live) {
  ReadStartMetadata(live.after);
  if (live.after.read)
    for (size_t i = 0; i < 2; ++i) ReadStartShaderPayload(base, live.after.shader_bindings[i], i == 1, live.shaders_after[i]);
  const auto after = StartPhysical(base, live.atlas_physical, 1024 * 256);
  live.atlas_after_equal = live.atlas_read && after.size() == 1024 * 256 &&
                           Sha256Hex(after.data(), after.size()) == live.atlas_sha;
}

// Isolated execution of the accepted occurrence: the same five requests,
// with only the color target swapped for an own 1280 x 720 RGBA8 surface
// cleared to [0, 0, 0, 255], through the same DrawIndexed; read back by
// ReadSurface once the fence of the fifth isolated draw has completed. The
// surface has the internal resolution scale, as every own surface; the
// comparator takes the 1x image only.
constexpr uint32_t kStartIsolatedKey = 0x00004200;  // No guest header lives in page 0.
struct StartIsolated {
  bool ok = false;
  uint32_t width = 0, height = 0;  // The own surface, scaled.
  std::string record, rgba_sha, rgba_file, failure;
  uint64_t clear_fence = 0, read_fence = 0;
  std::array<uint64_t, 5> fences{};
  uint64_t nonclear = 0;
  std::array<uint32_t, 4> bbox{};
};
StartIsolated RunStartIsolated(const std::vector<StartDraw>& lot) {
  StartIsolated out;
  std::vector<DrawRequest> copies;
  copies.reserve(lot.size());
  for (const auto& l : lot) {
    if (!l.request) {
      out.failure = "request-not-retained";
      out.record = "null";
      return out;
    }
    copies.push_back(*l.request);
    copies.back().color_key = kStartIsolatedKey;
  }
  std::vector<uint8_t> rgba;
  uint32_t width = 0, height = 0;
  std::string ensure_ev, clear_ev, read_ev, evidence;
  std::array<std::string, 5> draw_ev;
  auto* dispatcher = host::ActiveDispatcher();
  const bool ran = dispatcher && copies.size() == 5 && dispatcher->Run([&](Engine& engine, std::string& o) {
    SurfaceDesc desc;
    desc.key = kStartIsolatedKey;
    desc.width = 1280;
    desc.height = 720;
    desc.samples = 1;
    desc.format = SurfaceFormat::Color8888;
    if (!engine.EnsureSurface(desc, ensure_ev)) { o = ensure_ev; return false; }
    ClearRequest clear;
    clear.color_keys[0] = kStartIsolatedKey;
    clear.clear_color = true;
    clear.color = {0, 0, 0, 1};
    if (!engine.ClearSurfaces(clear, out.clear_fence, clear_ev)) { o = clear_ev; return false; }
    for (size_t i = 0; i < copies.size(); ++i)
      if (!engine.DrawIndexed(copies[i], out.fences[i], draw_ev[i])) { o = draw_ev[i]; return false; }
    if (!engine.ReadSurface(kStartIsolatedKey, out.fences[4], rgba, width, height, read_ev)) { o = read_ev; return false; }
    o = "{\"completed\":true}";
    return true;
  }, evidence);
  ForceFullConstants();  // The copies replaced the engine's constant blocks.
  std::string draws;
  bool same_requests = true;
  for (size_t i = 0; i < copies.size(); ++i) {
    const std::string isolated_sha = StartRequestSha(copies[i]);
    same_requests = same_requests && isolated_sha == lot[i].request_sha && !lot[i].request_sha.empty();
    draws += fmt::format("{}{{\"own_draw\":{},\"request_sha256_normal\":\"{}\",\"request_sha256_isolated\":\"{}\","
                         "\"equal\":{},\"target_normal\":{},\"target_isolated\":{},\"fence_normal\":{},"
                         "\"fence_isolated\":{},\"engine\":{}}}", i ? "," : "", lot[i].n, lot[i].request_sha,
                         isolated_sha, isolated_sha == lot[i].request_sha, lot[i].request->color_key,
                         copies[i].color_key, lot[i].fence, out.fences[i], draw_ev[i].empty() ? "null" : draw_ev[i]);
  }
  // Coverage as the comparator defines it (any channel other than the clear),
  // recorded as information only.
  out.bbox = {UINT32_MAX, UINT32_MAX, 0, 0};
  const ResolutionScale& scale = CurrentSettings().resolution_scale;
  out.width = scale.Width(1280);
  out.height = scale.Height(720);
  if (ran && width == out.width && height == out.height && rgba.size() == size_t(width) * height * 4) {
    for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width; ++x) {
        const uint8_t* p = &rgba[(size_t(y) * width + x) * 4];
        if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 255) continue;
        ++out.nonclear;
        out.bbox = {std::min(out.bbox[0], x), std::min(out.bbox[1], y), std::max(out.bbox[2], x + 1),
                    std::max(out.bbox[3], y + 1)};
      }
    out.rgba_sha = Sha256Hex(rgba.data(), rgba.size());
    const char* dir = std::getenv("REX_NATIVE_FRAME_DIR");
    if (dir && *dir) {
      const std::string name = fmt::format("start-isolated-{}x{}.rgba", width, height);
      const std::string path = fmt::format("{}/{}", dir, name);
      std::FILE* file = std::fopen(path.c_str(), "wb");
      const bool written = file && std::fwrite(rgba.data(), 1, rgba.size(), file) == rgba.size();
      if (file) std::fclose(file);
      if (written) out.rgba_file = name;
    }
    out.ok = same_requests && !out.rgba_sha.empty() && !out.rgba_file.empty();
    if (!out.ok) out.failure = same_requests ? "rgba-not-written" : "request-identity-differs";
  } else {
    out.failure = ran ? "readback-shape" : "engine-operation";
  }
  if (!out.nonclear) out.bbox = {0, 0, 0, 0};
  out.read_fence = 0;
  const auto read_at = read_ev.find("\"read_fence\":");
  if (read_at != std::string::npos) out.read_fence = std::strtoull(read_ev.c_str() + read_at + 13, nullptr, 10);
  out.record = fmt::format(
      "\"ok\":{},\"failure\":\"{}\",\"target\":{{\"key\":{},\"size\":[{},{}],\"format\":\"R8G8B8A8_UNORM\","
      "\"samples\":1,\"clear\":[0,0,0,255]}},\"same_requests_same_draw_indexed\":{},\"ensure\":{},\"clear\":{},"
      "\"clear_fence\":{},\"draws\":[{}],\"read\":{},\"read_after_fence\":{},\"rgba\":{{\"file\":\"{}\","
      "\"bytes\":{},\"sha256\":\"{}\",\"nonclear_pixels\":{},\"bbox\":[{}]}},\"dispatcher\":{}",
      out.ok, out.failure, kStartIsolatedKey, out.width, out.height, same_requests, ensure_ev.empty() ? "null" : ensure_ev,
      clear_ev.empty() ? "null" : clear_ev, out.clear_fence, draws, read_ev.empty() ? "null" : read_ev, out.fences[4],
      out.rgba_file, rgba.size(), out.rgba_sha, out.nonclear, fmt::join(out.bbox, ","),
      evidence.empty() ? "null" : evidence);
  return out;
}


void CountStartDraw(std::string_view vs_name, std::string_view ps_name) {
  if (vs_name == "vs-00055311" && ps_name == "ps-002C60A9" && !start_draws.fetch_add(1))
    start_first_swap.store(swaps.load());
}
void NoteStartDraw(PPCContext& ctx, uint8_t* base, uint32_t dev, const StartInputs& in) {
  // Origin: the producer arrival of this thread that tail-called the draw.
  const ProducerBefore before = producer_before;
  producer_before.drawn = true;
  if (MeasureSwaps() > 0) {  // The measurement mode: the lot is only counted.
    CountStartDraw(in.vs_name, in.ps_name);
    return;
  }
  if (in.vs_name != "vs-00055311" || in.ps_name != "ps-002C60A9") {
    std::lock_guard lock(start_lot.mutex);
    RejectStartRun("broken-by-other-draw");
    return;
  }
  if (!start_draws.fetch_add(1)) start_first_swap.store(swaps.load());
  const auto& r = in.request;
  StartDraw d;
  d.n = in.n;
  d.swap = swaps.load();
  d.thread = in.thread;
  d.producer_thread_call = before.thread_call;
  // Constants as the own request copied them; the variation B1 is already
  // put back in the guest state when this runs.
  for (unsigned i = 0; i < 4; ++i) d.c40[i] = HostWord(r.vs_constants, 160 + i);
  d.vertices = StartPhysical(base, in.stream0[0], 1024);
  d.indices = StartPhysical(base, in.index_physical, 192);
  d.vs_constants = GuestConstantBytes(r.vs_constants);
  d.ps_constants = GuestConstantBytes(r.ps_constants);
  auto request_words = [&](size_t first, size_t count) {
    std::string out;
    for (size_t i = 0; i < count; ++i) out += fmt::format("{}{}", i ? "," : "", HostWord(r.vs_constants, first + i));
    return out;
  };
  std::array<uint32_t, 6> f0{};
  for (unsigned i = 0; i < 6; ++i) f0[i] = Get32(base, dev + 0x480 + 4 * i);
  const uint32_t header = Get32(base, dev + 12432);
  ResourceRegistry::Entry target{};
  const bool target_known = resources.Find(header, target);
  const uint32_t global_device = Get32(base, 0x82C0F008);
  const uint32_t device_state = global_device ? Get32(base, global_device + 0x10) : 0;
  bool c38_c39 = true;
  for (unsigned i = 0; i < 4; ++i)
    c38_c39 = c38_c39 && HostWord(r.vs_constants, 152 + i) == kOneWord && HostWord(r.vs_constants, 156 + i) == 0;
  std::string failed;
  auto need = [&](bool ok, const char* name) {
    if (!ok) failed += fmt::format("{}\"{}\"", failed.empty() ? "" : ",", name);
  };
  need(in.indexed && in.lr == kStartLr, "entry-lr");
  need(!before.drawn && before.arrival, "producer-origin");
  need(in.primitive == 4 && in.index_count == 96 && !r.index32 && in.index_endian == 1, "indices");
  need(in.base_vertex == 0 && in.max_index == 63, "vertex-range");
  static const std::vector<VertexElement> no_elements;
  const auto& el = r.elements ? *r.elements : no_elements;
  need(el.size() == 2 && r.streams.size() == 1 && in.stream_fetch.size() == 1 && in.stream_fetch[0] == 95 &&
       r.streams[0].stride == 16 && in.stream0[2] == 2 && std::string_view(el[0].semantic) == "POSITION" &&
       !el[0].semantic_index && el[0].format == VertexFormat::kR32G32Float && !el[0].slot &&
       !el[0].offset && std::string_view(el[1].semantic) == "TEXCOORD" &&
       !el[1].semantic_index && el[1].format == VertexFormat::kR32G32Float && !el[1].slot &&
       el[1].offset == 8, "vertex-layout");
  need(in.stream0[1] >= 1024 && d.vertices.size() == 1024 && d.indices.size() == 192, "extent");
  need(f0[0] == 0x08000002 && (f0[1] & 0xFFF) == 2 && f0[2] == 0x001FE3FF && f0[3] == 0x00A80248 && f0[4] == 3 &&
       f0[5] == 0x218, "fetch0");
  need(r.textures.size() == 1 && !r.textures[0].reg && !r.textures[0].resolved &&
       r.textures[0].format == TextureFormat::kR8Unorm && r.textures[0].width == 1024 && r.textures[0].height == 256 &&
       r.textures[0].key == (f0[1] & 0xFFFFF000u), "atlas");
  need(in.depth_control == 0x00700264 && in.blend_control == 0x07000706 && in.color_control == 0xAA000004 &&
       in.mode == 0x18000 && in.vte == 0x43F && in.color_mask == 0xF, "state");
  need(r.viewport == std::array<float, 6>{0, 0, 1280, 720, 0, 1} && r.scissor == std::array<int32_t, 4>{0, 0, 1280, 720},
       "viewport-scissor");
  need(r.color_key == header && !r.depth_key && target_known && target.kind == ResourceRegistry::Kind::Surface &&
       target.args[0] == 1280 && target.args[1] == 720 && !target.args[3], "target");
  need(device_state == dev, "device-state");
  need(c38_c39, "c38-c39");
  need(StartRed(d.c40) || StartWhite(d.c40), "c40");
  // The contract's VS route (first draw reused, the other four
  // inherited, checked on the run below) and its fixed cache words.
  const std::string vs_route = StartVsRoute(in.before);
  d.route = vs_route;
  need(vs_route == "cached-base-fast-reuse" || vs_route == "inherited", "vs-route");
  need(StartContractCache(in.before), "contract-cache");
  const uint64_t vertices_fnv = StartFnv(d.vertices), indices_fnv = StartFnv(d.indices);
  const bool c40_a1 = StartWhite(d.c40) || d.c40 == std::array<uint32_t, 4>{kStartRedWord, 0, 0, kOneWord};
  d.record = fmt::format(
      "\"own_draw\":{},\"swap\":{},\"fence\":{},\"guest_thread\":{},\"lr\":{},\"r1\":{},"
      "\"producer\":{{\"call\":{},\"arrival\":{},\"thread_call\":{},\"taken_by_this_draw\":{},\"r3\":{},\"r4\":{},"
      "\"r5\":{},\"r1\":{}}},\"index_count\":{},\"start_index\":{},\"base_vertex\":{},\"max_index\":{},"
      "\"vertices\":{{\"address\":{},\"extent_bytes\":{},\"endian\":{},\"read\":{},\"fnv\":\"{:016x}\",\"a1_equal\":{}}},"
      "\"indices\":{{\"descriptor\":{},\"physical\":{},\"endian\":{},\"read\":{},\"fnv\":\"{:016x}\",\"a1_equal\":{}}},"
      "\"fetch0\":[{}],\"c0_c3\":[{}],\"c38_c40\":[{}],\"c40_class\":\"{}\",\"c40_a1_equal\":{},"
      "\"vs_constants_fnv\":\"{:016x}\",\"ps_constants_fnv\":\"{:016x}\",\"dirty_before\":[{}],"
      "\"signature_failed\":[{}]", d.n, d.swap, in.fence, in.thread, in.lr, ctx.r1.u32, before.call, before.arrival,
      before.thread_call, !before.drawn, before.r3, before.r4, before.r5, before.r1, in.index_count, in.start_index,
      in.base_vertex, in.max_index, in.stream0[0], in.stream0[1], in.stream0[2], d.vertices.size(), vertices_fnv,
      vertices_fnv == kStartVerticesFnv, in.ib_descriptor, in.index_physical, in.index_endian, d.indices.size(),
      indices_fnv, indices_fnv == kStartIndicesFnv, fmt::join(f0, ","), request_words(0, 16),
      request_words(152, 12), StartWhite(d.c40) ? "white" : StartRed(d.c40) ? "red" : "other",
      c40_a1, StartFnv(d.vs_constants), StartFnv(d.ps_constants), fmt::join(in.dirty_before, ","), failed);
  // The producer item (r4, 224 bytes) and the atlas bytes at this draw.
  d.vertex_address = in.stream0[0];
  d.index_physical = in.index_physical;
  d.atlas_physical = f0[1] & 0xFFFFF000u;
  d.atlas_fnv = StartFnv(StartPhysical(base, d.atlas_physical, 1024 * 256));
  d.record += fmt::format(",\"item\":{{\"address\":{},\"hex\":\"{}\"}},\"atlas_fnv\":\"{:016x}\",\"before_read\":{},"
                          "\"vs_route\":\"{}\"", before.r4, GuestHex(base, before.r4, 224), d.atlas_fnv, in.before.read,
                          vs_route);
  d.before = StartMetadataJson(in.before);
  if (start_census.fetch_add(1) < 64) host::Record("native-d3d-start-census", d.record);
  if (failed.empty()) {
    // The request as executed, its identity and copies with the fence,
    // and the payload record.
    d.request = std::make_shared<const DrawRequest>(r);
    d.request_sha = StartRequestSha(r);
    d.fence = in.fence;
    d.copies = StartCopiesJson(r, in.fence);
    d.policy = in.live.policy;
    d.variation = in.live.variation_record;
    const auto& live = in.live;
    std::vector<uint8_t> vertices_le = d.vertices, indices_le = d.indices;
    const bool swapped = SwapEndian(vertices_le, 2) && SwapEndian(indices_le, 1);
    bool colors_equal = true;
    for (size_t i = 0; i < 12; ++i)
      colors_equal = colors_equal && in.before.colors_c38_c40[i] == HostWord(r.vs_constants, 152 + i);
    const bool bindings_equal = in.before.read && swapped && colors_equal &&
        std::equal(f0.begin(), f0.end(), in.before.fetch_words.begin()) && in.before.device == global_device &&
        in.before.index_descriptor == in.ib_descriptor &&
        in.before.shader_bindings == std::array<uint32_t, 2>{in.ps.object, in.vs.object} && !r.streams.empty() &&
        r.streams[0].data.size() >= vertices_le.size() && vertices_le.size() == 1024 &&
        std::equal(vertices_le.begin(), vertices_le.end(), r.streams[0].data.begin()) && indices_le == r.indices;
    StartMetadata repeat = live.repeat;
    constexpr size_t kSpinlock = (0x2A98 - 0x2780) / 4;  // Kernel spinlock word, excluded as by the normalizer.
    if (repeat.read) repeat.render_cache_2780[kSpinlock] = in.before.render_cache_2780[kSpinlock];
    const bool repeat_equal = repeat.read && in.before.read && StartMetadataJson(repeat) == StartMetadataJson(in.before);
    const bool shaders_read = live.shaders[0].read && live.shaders[1].read;
    const bool shaders_repeat_equal = shaders_read && live.shaders == live.shaders_repeat;
    const bool shaders_after_equal = shaders_repeat_equal && live.shaders_after == live.shaders &&
                                     live.after.read && live.after.shader_bindings == in.before.shader_bindings;
    const uint32_t valid = (in.before.read ? 1u : 0u) | (live.atlas_read ? 2u : 0u) | (bindings_equal ? 4u : 0u) |
                           (live.atlas_repeat_equal && repeat_equal ? 8u : 0u) | (live.atlas_read ? 16u : 0u) |
                           (live.after.read ? 32u : 0u) | (live.atlas_after_equal ? 64u : 0u);
    const uint32_t shader_valid = (shaders_repeat_equal ? 3u : 0u) | (shaders_after_equal ? 4u : 0u);
    d.n5 = fmt::format(
        "{{\"candidate_at_entry\":{},\"valid\":{},\"shader_valid\":{},\"bindings_equal\":{},\"repeat_equal\":{},"
        "\"atlas\":{{\"physical\":{},\"sha256\":\"{}\",\"read\":{},\"repeat_equal\":{},\"after_equal\":{}}},"
        "\"shaders\":[{},{}],\"shaders_repeat_equal\":{},\"shaders_after_equal\":{},\"repeat\":{},\"after\":{}}}",
        live.candidate, valid, shader_valid, bindings_equal, repeat_equal, live.atlas_physical, live.atlas_sha,
        live.atlas_read, live.atlas_repeat_equal, live.atlas_after_equal, StartShaderPayloadJson(live.shaders[0], false),
        StartShaderPayloadJson(live.shaders[1], true), shaders_repeat_equal, shaders_after_equal,
        StartMetadataJson(live.repeat), StartMetadataJson(live.after));
  }
  std::vector<StartDraw> lot;
  bool preceded = false;
  uint64_t occurrence = 0;
  {
    std::lock_guard lock(start_lot.mutex);
    if (!failed.empty()) {
      RejectStartRun("broken-by-signature");
      return;
    }
    if (!start_lot.run.empty() && (start_lot.run.back().n + 1 != d.n || start_lot.run.back().thread != d.thread ||
                                   start_lot.run.back().producer_thread_call + 1 != d.producer_thread_call))
      RejectStartRun("not-contiguous");
    const bool white = StartWhite(d.c40);
    start_lot.run.push_back(std::move(d));
    if (!white) return;
    auto& run = start_lot.run;
    if (run.size() != 5 || !StartRed(run[0].c40) || run[1].c40 != run[0].c40 || run[2].c40 != run[0].c40 ||
        run[3].c40 != run[0].c40) {
      RejectStartRun(run.size() < 5 ? "white-before-four-reds" : run.size() > 5 ? "longer-than-five" : "reds-differ");
      return;
    }
    if (run[0].route != "cached-base-fast-reuse" ||
        std::any_of(run.begin() + 1, run.end(), [](const StartDraw& s) { return s.route != "inherited"; })) {
      RejectStartRun("vs-route-not-reuse-then-inherited");
      return;
    }
    preceded = start_lot.last_run_end + 1 == run[0].n;
    start_lot.last_run_end = run.back().n;
    lot = std::move(run);
    run.clear();
    occurrence = ++start_lot.occurrences;
  }
  if (occurrence < StartStopOccurrence()) {
    std::string draws;
    for (const auto& l : lot)
      draws += fmt::format("{}{{\"own_draw\":{},\"swap\":{},\"vertices_a1\":{},\"indices_a1\":{},\"c40\":[{}]}}",
                           draws.empty() ? "" : ",", l.n, l.swap, StartFnv(l.vertices) == kStartVerticesFnv,
                           StartFnv(l.indices) == kStartIndicesFnv, fmt::join(l.c40, ","));
    host::Record("native-d3d-start-lot", fmt::format(
        "\"occurrence\":{},\"stop_occurrence\":{},\"preceded_by_signature_draw\":{},\"draws\":[{}],\"continues\":true",
        occurrence, StartStopOccurrence(), preceded, draws));
    return;
  }
  // The occurrence. The varied run must be exactly this lot, and the five
  // requests are drawn again into the isolated target and read back.
  std::vector<uint64_t> lot_draws;
  for (const auto& l : lot) lot_draws.push_back(l.n);
  if (StartVariationMode() == 1) {
    std::vector<uint64_t> varied;
    bool finished = false;
    {
      std::lock_guard lock(start_variation.mutex);
      varied = start_variation.own_draws;
      finished = start_variation.finished;
    }
    if (!finished || varied != lot_draws)
      RejectStartVariation(ctx, "accepted-lot-differs-from-varied-run",
                           fmt::format("\"lot\":[{}],\"varied\":[{}]", fmt::join(lot_draws, ","), fmt::join(varied, ",")));
  }
  const StartIsolated isolated = RunStartIsolated(lot);
  host::Record("native-d3d-start-isolated", fmt::format(
      "\"occurrence\":{},\"own_draws\":[{}],{},\"policy\":{},\"variation\":{}", occurrence, fmt::join(lot_draws, ","),
      isolated.record, StartPolicyJson(), StartVariationJson()));
  if (!isolated.ok) StopInOrder(ctx, "native-d3d-start-isolated-unproved", 88);
  // Evidence of the hand-off at the fifth draw.
  using StartBlob = std::pair<const char*, const std::vector<uint8_t>*>;
  std::string draws, files, own;
  bool vertices_a1 = true, indices_a1 = true, red_a1 = lot[0].c40 == std::array<uint32_t, 4>{kStartRedWord, 0, 0, kOneWord};
  std::string retention;
  bool retained = true;
  for (size_t i = 0; i < lot.size(); ++i) {
    const auto& l = lot[i];
    draws += fmt::format("{}{{{},\"before\":{},\"request_sha256\":\"{}\",\"copies\":{},\"n5\":{},\"variation\":{},"
                         "\"policy\":{}}}", i ? "," : "", l.record, l.before, l.request_sha,
                         l.copies.empty() ? "null" : l.copies, l.n5.empty() ? "null" : l.n5, l.variation, l.policy);
    // The bytes each draw read, read again at the fifth draw.
    const bool v = StartFnv(StartPhysical(base, l.vertex_address, 1024)) == StartFnv(l.vertices);
    const bool x = StartFnv(StartPhysical(base, l.index_physical, 192)) == StartFnv(l.indices);
    const bool a = StartFnv(StartPhysical(base, l.atlas_physical, 1024 * 256)) == l.atlas_fnv;
    retained = retained && v && x && a;
    retention += fmt::format("{}{{\"own_draw\":{},\"vertices\":{},\"indices\":{},\"atlas\":{}}}",
                             i ? "," : "", l.n, v, x, a);
    own += fmt::format("{}{}", i ? "," : "", l.n);
    vertices_a1 = vertices_a1 && StartFnv(l.vertices) == kStartVerticesFnv;
    indices_a1 = indices_a1 && StartFnv(l.indices) == kStartIndicesFnv;
    for (const auto& [suffix, bytes] : std::initializer_list<StartBlob>{{"vertices.bin", &l.vertices},
             {"indices.bin", &l.indices}, {"vs-constants.bin", &l.vs_constants}, {"ps-constants.bin", &l.ps_constants}})
      files += fmt::format("{}\"{}\"", files.empty() ? "" : ",", WriteStartFile(fmt::format("draw{}-{}", i, suffix), *bytes));
  }
  const uint32_t atlas_physical = f0[1] & 0xFFFFF000u;
  const auto atlas = StartPhysical(base, atlas_physical, 1024 * 256);
  const auto vs_code = StartPhysical(base, in.vs.code_physical, in.vs.code_size);
  const auto ps_code = StartPhysical(base, in.ps.code_physical, in.ps.code_size);
  for (const auto& [name, bytes] : std::initializer_list<StartBlob>{{"atlas.bin", &atlas}, {"vs.ucode", &vs_code},
                                                                    {"ps.ucode", &ps_code}})
    files += fmt::format(",\"{}\"", WriteStartFile(name, *bytes));
  const uint64_t atlas_nonzero = std::count_if(atlas.begin(), atlas.end(), [](uint8_t b) { return b != 0; });
  // The header of the atlas is owners.atlas_header (state+0x30F8).
  struct { uint64_t layout = 0, version = 0; } copy;  // The copy posted for the atlas base.
  const bool copy_known = TexturePosted(atlas_physical, copy.layout, copy.version);
  const uint32_t texture_descriptor = Get32(base, dev + 0x30F8), depth_header = Get32(base, dev + 12448);
  // Owners of the bound inputs and the lifetime evidence available here.
  const std::string owners = fmt::format(
      "{{\"index_header\":{},\"atlas_header\":{},\"vertex_headers\":{},\"producer_item\":{{\"address\":{},"
      "\"hex\":\"{}\"}}}}", StartHeader(base, in.ib_descriptor, 32), StartHeader(base, texture_descriptor, 52),
      StartVertexHeaders(base, dev, in.stream0[0], in.stream0[0] + in.stream0[1]), before.r4,
      GuestHex(base, before.r4, 224));
  host::Record("native-d3d-start-occurrence", fmt::format(
      "\"lot\":{{\"occurrence\":{},\"own_draws\":[{}],\"contiguous\":true,\"pattern\":\"four equal reds then white\","
      "\"preceded_by_signature_draw\":{},\"following_draw_executed\":false,\"rejected_runs\":{},\"census\":{}}},"
      "\"draws\":[{}],\"a1\":{{\"vertices_equal\":{},\"indices_equal\":{},\"red_words_equal\":{},\"white\":true}},"
      "\"shaders\":{{{},\"vs_code\":{{\"physical\":{},\"bytes\":{},\"fnv\":\"{:016x}\"}},"
      "\"ps_code\":{{\"physical\":{},\"bytes\":{},\"fnv\":\"{:016x}\"}}}},"
      "\"atlas\":{{\"fetch0\":[{}],\"physical\":{},\"read\":{},\"fnv\":\"{:016x}\",\"nonzero\":{},\"headers_by\":\"owners.atlas_header\","
      "\"own_copy\":{{\"known\":{},\"layout\":{},\"version\":{}}},\"textures\":[{}]}},"
      "\"descriptors\":{{\"texture_30F8\":{{\"address\":{},\"words\":[{}]}},\"index_308C\":{{\"address\":{},\"words\":[{}]}}}},"
      "\"state\":{{{},\"viewport\":[{}],\"scissor\":[{}]}},"
      "\"target\":{{\"header\":{},\"key\":{},\"generation\":{},\"args\":[{}],\"samples\":{},\"header_hex\":\"{}\","
      "\"depth_header\":{},\"depth_bound\":false}},"
      "\"device\":{{\"global_82C0F008\":{},\"state\":{},\"dev\":{},\"cursor_before\":[{}],\"guard_cursor\":{},"
      "\"cursor_after\":[{},{},{}],\"inline_scope_thread\":{},\"discard_thread\":{}}},"
      "\"files\":[{}],\"engine\":{},\"retention\":{{\"all\":{},\"draws\":[{}]}},\"owners\":{},"
      "\"isolated\":{{\"file\":\"{}\",\"rgba_sha256\":\"{}\",\"clear_fence\":{},\"fences\":[{}],\"read_fence\":{}}},"
      "\"policy\":{},\"variation\":{}", occurrence, own, preceded, start_lot.rejected, start_census.load(), draws, vertices_a1,
      indices_a1, red_a1, in.shaders_record, in.vs.code_physical, vs_code.size(), StartFnv(vs_code), in.ps.code_physical,
      ps_code.size(), StartFnv(ps_code), fmt::join(f0, ","), atlas_physical, atlas.size(), StartFnv(atlas),
      atlas_nonzero, copy_known, copy.layout, copy.version, in.textures_record, texture_descriptor,
      texture_descriptor ? StartWords(base, texture_descriptor, 13) : "", in.ib_descriptor,
      in.ib_descriptor ? StartWords(base, in.ib_descriptor, 8) : "", in.state_record,
      fmt::join(r.viewport, ","), fmt::join(r.scissor, ","), header, r.color_key, target.generation,
      fmt::join(target.args, ","), resources.Samples(header), GuestHex(base, header, 52), depth_header,
      global_device, device_state, dev, fmt::join(in.cursor_before, ","), DeviceOwner::kCursor,
      Get32(base, dev + 48), Get32(base, dev + 52), Get32(base, dev + 56), inline_draw.scope_thread.load(),
      discard_thread.load(), files, in.engine, retained, retention, owners, isolated.rgba_file, isolated.rgba_sha,
      isolated.clear_fence, fmt::join(isolated.fences, ","), isolated.read_fence, StartPolicyJson(),
      StartVariationJson()));
  // Provenance fields that the stop writes with the journal, executable
  // and source identity hashes (live-provenance.json).
  std::string fences_normal, request_shas;
  for (const auto& l : lot) {
    fences_normal += fmt::format("{}{}", fences_normal.empty() ? "" : ",", l.fence);
    request_shas += fmt::format("{}\"{}\"", request_shas.empty() ? "" : ",", l.request_sha);
  }
  host::SetStopProvenance(fmt::format(
      "\"stop\":\"native-d3d-start-occurrence\",\"occurrence\":{{\"number\":{},\"own_draws\":[{}],\"swap\":{},"
      "\"fences_normal\":[{}],\"clear_fence\":{},\"fences_isolated\":[{}],\"read_fence\":{},"
      "\"request_sha256\":[{}]}},\"rgba\":{{\"file\":\"{}\",\"bytes\":{},\"width\":{},\"height\":{},"
      "\"format\":\"R8G8B8A8_UNORM\",\"sha256\":\"{}\"}},\"policy\":{},\"variation\":\"{}\"",
      occurrence, fmt::join(lot_draws, ","), lot.back().swap, fences_normal, isolated.clear_fence,
      fmt::join(isolated.fences, ","), isolated.read_fence, request_shas, isolated.rgba_file,
      size_t(isolated.width) * isolated.height * 4, isolated.width, isolated.height, isolated.rgba_sha, StartPolicyJson(), StartVariationMode() == 1 ? "B1" : "none"));
  StopInOrder(ctx, "native-d3d-start-occurrence", 86);
}
}  // namespace gpu

// Pacing samples (REX_NATIVE_PACING_SAMPLES=N, 1 to 20000), alike in the
// own mode and in the reference (Xenos) mode, without Frida. At the entry of
// every DrawIndexedVertices, a START lot candidate (StartEntryCandidate, the
// device state the original commit reads) gives the animation counter: the
// red of c40, the pulse of the START text. The frame layer samples it at each
// Swap entry with the host time and the device counters (PacingAtSwap).
namespace gpu {
uint64_t PacingSamples() {
  static const uint64_t n = [] {
    if (!Diagnostics()) return uint64_t(0);
    const char* v = std::getenv("REX_NATIVE_PACING_SAMPLES");
    if (!v || !*v) return uint64_t(0);
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(v, &end, 10);
    return end && !*end && parsed >= 1 && parsed <= 20000 ? uint64_t(parsed) : UINT64_MAX;  // MAX: invalid.
  }();
  return n;
}
std::atomic<uint32_t> pacing_red_word{0}, pacing_red{0}, pacing_white{0};  // Frame being drawn.
std::atomic<bool> pacing_content{false};
void NotePacingDraw(uint8_t* base, uint32_t dev, uint32_t lr, uint32_t primitive, uint32_t index_count) {
  if (!PacingSamples() || !StartEntryCandidate(base, dev, true, lr, primitive, index_count)) return;
  std::array<uint32_t, 4> c40{};
  for (unsigned i = 0; i < 4; ++i) c40[i] = Get32(base, dev + 0x780 + 16 * 40 + 4 * i);
  if (!StartRed(c40)) {
    pacing_white.fetch_add(1);
    return;
  }
  if (!pacing_red.fetch_add(1)) pacing_red_word.store(c40[0]);
  pacing_content.store(true);
}
}  // namespace gpu
