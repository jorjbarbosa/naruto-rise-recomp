#pragma once
#include <span>

// Evidence of the START lot (start_evidence.cpp), a diagnostic of the title
// screen: its census and occurrence stop, the live inputs and the isolated
// target of the image comparator, the policy of shaders and sampling, the
// variation B1 and the pacing samples.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <rex/ppc/context.h>
#include "gpu/backend/engine.h"
#include "gpu/shaders/shaders.h"

namespace gpu {
constexpr uint32_t kStartLr = 0x82108330, kOneWord = 0x3F800000, kStartRedWord = 0x3F23A3A4;
// The stage-2 input metadata of before.json (ReadInputMetadata of
// historical/native_renderer_probe.cpp, same reads and names), read at the
// DrawIndexedVertices entry. 821024A8 only loads its item before the tail call,
// so this is the state at BEFORE 821024A8/LR 82108330.
struct StartMetadata {
  bool read = false;
  uint32_t device = 0, state = 0, texture_descriptor = 0, index_descriptor = 0, shader_context = 0;
  uint32_t shader_context_count = 0, recycle_descriptor = 0, recycle_base = 0, recycle_base_observed = 0;
  uint32_t target_mode_3370 = 0, shader_mode_34b0 = 0;
  std::array<uint32_t, 12> dirty_words{}, colors_c38_c40{};
  std::array<uint32_t, 16> matrix_c0_c3{}, shader_dependencies_2e20{};
  std::array<uint32_t, 192> fetch_words{};
  std::array<uint32_t, 208> render_cache_2780{};
  std::array<uint32_t, 13> texture_descriptor_words{};
  std::array<uint32_t, 8> index_descriptor_words{}, target_dependencies_31b0{};
  std::array<uint32_t, 2> shader_bindings{};
  std::array<uint32_t, 5> shader_context_words_20{}, target_dependencies_3090{};
  std::array<uint32_t, 4> shader_reuse_30e8{};
  std::array<uint32_t, 7> vs_cache_words_28{};
  std::array<std::array<uint32_t, 4>, 5> target_descriptor_words_18{};
};
struct StartVariationDraw {
  bool applied = false;
  size_t k = 0;
  uint32_t a = 0, b = 0, observed_a = 0, dirty_before = 0, dirty_applied = 0, observed_b = 0;
};
// Shader payload of the lot: the container header and metadata words of the
// bound object and the physical microcode they point to. Read at the entry
// (before and repeat) and after the original commit.
struct StartShaderPayload {
  bool read = false;
  uint32_t binding = 0, relative_metadata = 0, code_base = 0, metadata_address = 0, physical = 0;
  std::array<uint32_t, 8> container_header{}, metadata{};
  std::vector<uint8_t> bytes;
  bool operator==(const StartShaderPayload&) const = default;
};
// Entry and after-commit reads of a lot candidate: metadata, repeat, shaders
// and atlas.
struct StartLiveInputs {
  bool candidate = false;
  StartMetadata repeat, after;
  std::array<StartShaderPayload, 2> shaders, shaders_repeat, shaders_after;  // PS, VS
  uint32_t atlas_physical = 0;
  std::string atlas_sha;
  bool atlas_read = false, atlas_repeat_equal = false, atlas_after_equal = false;
  StartVariationDraw variation;
  std::string policy = "null", variation_record = "null";
};
// Inputs of the own draw that the signature reads.
struct StartInputs {
  bool indexed;
  uint32_t lr, thread, primitive;
  int32_t base_vertex;
  uint32_t start_index, index_count, max_index;
  const DrawRequest& request;
  const std::vector<uint32_t>& stream_fetch;
  std::array<uint32_t, 3> stream0;  // Address, bytes, endian.
  uint32_t ib_descriptor, index_physical, index_endian;
  const BoundShader& vs;
  const BoundShader& ps;
  std::string_view vs_name, ps_name, shaders_record, textures_record, state_record, engine;
  uint32_t depth_control, blend_control, color_control, mode, vte, color_mask;
  std::array<uint32_t, 12> dirty_before;
  std::array<uint32_t, 3> cursor_before;  // +48, +52, +56 before the original commit.
  uint64_t n, fence;
  const StartMetadata& before;
  const StartLiveInputs& live;
};
// Live validation of the START lot against a reference image.
//
// Policy of shaders and sampling, sealed by SHA before the comparative boot
// and given by REX_NATIVE_START_POLICY (a directory with policy.tsv, vs.dxil and
// ps.dxil). For draws whose shaders are the lot's (cache vs-00055311 and
// ps-002C60A9), the DXIL of the policy replaces the cache DXIL:
// vs_6_6/ps_6_6 with -Ges, the PS with ReferenceSampling (the host rounding
// convention of the Xenos reference, 1.5/1024 texel; neither guest data nor
// hardware fidelity). It is selected by data: only when the current physical
// microcode of the VS, already patched, and that of the PS equal the effective
// payloads of the policy; any difference is a guard. Without the variable the
// cache DXIL is used.
struct StartPolicy {
  std::once_flag once;
  bool requested = false, loaded = false;
  std::string error, dir, name, vs_cache, ps_cache, vs_ucode, ps_ucode, vs_dxil_sha, ps_dxil_sha, file_sha;
  std::shared_ptr<const std::vector<uint8_t>> vs_dxil, ps_dxil;
  std::atomic<uint64_t> selections{0};
};
extern StartPolicy start_policy;
// The lot's draw as the entry sees it, before the original commit.
bool StartEntryCandidate(uint8_t* base, uint32_t dev, bool indexed, uint32_t lr, uint32_t primitive,
                         uint32_t index_count);
bool StartRed(const std::array<uint32_t, 4>& c);
bool ReadStartMetadata(StartMetadata& m);
uint64_t StartStopOccurrence();
std::vector<uint8_t> StartPhysical(uint8_t* base, uint32_t physical, uint32_t size);
void LoadStartPolicy();
std::string StartPolicyJson();
std::vector<uint8_t> BigEndianBytes(const uint32_t* words, size_t count);
uint32_t HostWord(const std::array<uint8_t, 4096>& host, size_t index);
uint32_t HostWord(std::span<const uint8_t> host, size_t index);
int StartVariationMode();  // 0 none, 1 B1, -1 not recognized.
// At the entry of every own draw while B1 is asked, before the original commit.
StartVariationDraw ApplyStartVariation(PPCContext& ctx, uint8_t* base, uint32_t dev, bool candidate,
                                       const StartMetadata& pre, uint32_t thread);
// After the own draw: the exact A word back, the c0-c3 group rescheduled.
std::string RestoreStartVariation(PPCContext& ctx, uint8_t* base, uint32_t dev, const StartVariationDraw& d,
                                  uint64_t own_draw, uint32_t request_c0w);
void ReadStartEntry(uint8_t* base, const StartMetadata& before, StartLiveInputs& live);
void ReadStartAfter(uint8_t* base, StartLiveInputs& live);
void NoteStartDraw(PPCContext& ctx, uint8_t* base, uint32_t dev, const StartInputs& in);
// The measurement mode: the lot is only counted (the first START draw starts
// the measured frames); also what the deferred route notes on the title.
void CountStartDraw(std::string_view vs_name, std::string_view ps_name);
// Pacing samples (REX_NATIVE_PACING_SAMPLES).
uint64_t PacingSamples();
extern std::atomic<uint32_t> pacing_red_word, pacing_red, pacing_white;  // Frame being drawn.
extern std::atomic<bool> pacing_content;
void NotePacingDraw(uint8_t* base, uint32_t dev, uint32_t lr, uint32_t primitive, uint32_t index_count);
}  // namespace gpu
