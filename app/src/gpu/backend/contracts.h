// The contracts of the engine's operations (engine.h), the same for every
// backend (backend.h): the descriptions they take and the native result they
// report.
#pragma once
#include "gpu/common/draw_storage.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <array>
#include <cstdint>
#include <vector>
#include "gpu/common/frame_event.h"
#include "gpu/common/platform.h"
#include "gpu/settings.h"

namespace gpu {
// Resource layer of the D3D boundary. Keys are guest header
// addresses of XDK D3D resources; formats are the host equivalents.
// Sizes, rectangles, the viewport and sample counts of every request are
// the guest's; the engine maps them to its own surfaces by the RenderMode of
// Initialize.
enum class SurfaceFormat : uint32_t { Color8888, DepthStencil };
struct SurfaceDesc {
  uint32_t key = 0, width = 0, height = 0, guest_format = 0, samples = 1;
  SurfaceFormat format = SurfaceFormat::Color8888;
};
// One clear of the bound surfaces (D3DDevice_ClearF / BeginTiling semantics).
// Zero keys are unbound slots. An empty rect (right <= left) means the whole surface.
struct ClearRequest {
  std::array<uint32_t, 4> color_keys{};
  uint32_t depth_key = 0;
  bool clear_color = false, clear_depth = false, clear_stencil = false;
  std::array<float, 4> color{};
  float depth = 1.0f;
  uint8_t stencil = 0;
  int32_t left = 0, top = 0, right = 0, bottom = 0;
};

// D3DDevice_Resolve of one bound surface into a guest texture header.
struct ResolveRequest {
  uint32_t source_key = 0, dest_key = 0, dest_width = 0, dest_height = 0, dest_format = 0;
  bool has_rect = false;
  uint32_t left = 0, top = 0, right = 0, bottom = 0, dest_x = 0, dest_y = 0;
  // Source sample (RB_COPY_CONTROL.copy_sample_select k0..k3), or -1 for all
  // samples: their minimum for a depth source, their average for a color one.
  int32_t sample = -1;
};

// The formats of the engine's resources, the ones the game uses, by name.
// Their numbers are a fixed encoding: the vertex formats enter the layout ids
// and the pipeline keys (pipeline_keys.h, schema 1 of the embedded list) and
// the texture formats the diagnostic records, so a number never changes
// meaning. They coincide with DXGI_FORMAT; every backend still translates them
// through its own table (backend/d3d12/formats.h).
enum class VertexFormat : uint32_t {
  kUnknown = 0,
  kR32G32B32A32Float = 2,
  kR32G32B32A32Uint = 3,
  kR32G32B32Float = 6,
  kR32G32B32Uint = 7,
  kR16G16B16A16Float = 10,
  kR16G16B16A16Unorm = 11,
  kR16G16B16A16Uint = 12,
  kR16G16B16A16Snorm = 13,
  kR16G16B16A16Sint = 14,
  kR32G32Float = 16,
  kR32G32Uint = 17,
  kR8G8B8A8Unorm = 28,
  kR8G8B8A8Uint = 30,
  kR8G8B8A8Snorm = 31,
  kR8G8B8A8Sint = 32,
  kR16G16Float = 34,
  kR16G16Unorm = 35,
  kR16G16Uint = 36,
  kR16G16Snorm = 37,
  kR16G16Sint = 38,
  kR32Float = 41,
  kR32Uint = 42,
};
enum class TextureFormat : uint32_t {
  kUnknown = 0,
  kR32G32B32A32Float = 2,
  kR16G16B16A16Float = 10,
  kR32G32Float = 16,
  kR8G8B8A8Unorm = 28,
  kR32Float = 41,
  kR8G8Unorm = 49,
  kR8Unorm = 61,
  kBC1Unorm = 71,
  kBC3Unorm = 77,
  kBC5Unorm = 83,
  kB5G6R5Unorm = 85,
};
// Every texture format of the contract.
inline constexpr TextureFormat kTextureFormats[] = {
    TextureFormat::kR32G32B32A32Float, TextureFormat::kR16G16B16A16Float, TextureFormat::kR32G32Float,
    TextureFormat::kR8G8B8A8Unorm,     TextureFormat::kR32Float,          TextureFormat::kR8G8Unorm,
    TextureFormat::kR8Unorm,           TextureFormat::kBC1Unorm,          TextureFormat::kBC3Unorm,
    TextureFormat::kBC5Unorm,          TextureFormat::kB5G6R5Unorm};
// A format of 4 x 4 texel blocks.
constexpr bool IsBlockCompressed(TextureFormat format) {
  return format == TextureFormat::kBC1Unorm || format == TextureFormat::kBC3Unorm ||
         format == TextureFormat::kBC5Unorm;
}
// The bytes of one texel, or of one 4 x 4 block of the block formats.
constexpr uint32_t BlockBytes(TextureFormat format) {
  switch (format) {
    case TextureFormat::kR32G32B32A32Float: return 16;
    case TextureFormat::kR16G16B16A16Float:
    case TextureFormat::kR32G32Float:
    case TextureFormat::kBC1Unorm: return 8;
    case TextureFormat::kR8G8B8A8Unorm:
    case TextureFormat::kR32Float: return 4;
    case TextureFormat::kR8G8Unorm:
    case TextureFormat::kB5G6R5Unorm: return 2;
    case TextureFormat::kR8Unorm: return 1;
    case TextureFormat::kBC3Unorm:
    case TextureFormat::kBC5Unorm: return 16;
    case TextureFormat::kUnknown: break;
  }
  return 0;
}

// The rows of one level as the requests carry them (DrawTexture::data): packed
// rows of texels, or of 4 x 4 blocks for the block formats.
struct LevelRows {
  uint32_t rows = 0, row_bytes = 0;
};
constexpr LevelRows RowsOfLevel(TextureFormat format, uint32_t width, uint32_t height, uint32_t level) {
  const uint32_t w = width >> level ? width >> level : 1, h = height >> level ? height >> level : 1;
  if (IsBlockCompressed(format)) return {(h + 3) / 4, (w + 3) / 4 * BlockBytes(format)};
  return {h, w * BlockBytes(format)};
}

// One own indexed draw (D3DDevice_DrawIndexedVertices semantics).
// Shaders are XenosRecomp's with its binding layout: textures t0 in spaces
// 0..2, samplers s0 space3, constants b0 (VS), b1 (PS) and b2 (shared) in
// space4. Data is already little endian; textures are already linear. Xenos
// register values are translated by the engine: RB_BLENDCONTROL0,
// RB_COLOR_MASK and the depth/cull fields.
struct VertexElement {
  const char* semantic = "POSITION";  // Static string (XenosRecomp semantics).
  uint32_t semantic_index = 0;
  VertexFormat format = VertexFormat::kUnknown;
  uint32_t slot = 0, offset = 0;
};
struct VertexStream {
  std::vector<uint8_t> data;  // Inline data of the draw (key 0).
  uint32_t stride = 0;
  // A buffer the engine keeps (UploadBuffer), bound as the view of
  // bytes at offset; data stays empty.
  uint64_t key = 0;
  uint32_t offset = 0, bytes = 0;
};
// One upload of a buffer the engine keeps (vertex or index data,
// already little endian), keyed by the caller. Created at its first upload,
// whose single range covers it; later uploads copy their ranges in the open
// batch, before the draws recorded after them. A version not above the
// buffer's is skipped. Released by ReleaseBuffer once its last use completed.
struct BufferUpload {
  uint64_t key = 0, version = 0;
  uint32_t bytes = 0;
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> ranges;  // (offset, data)
};
// The source of one component a shader reads from a sampled texture: one of
// the texture's components or a constant, in the order of the Xenos fetch
// swizzle selectors. The mapping lists them for r, g, b and a.
enum class Swizzle : uint8_t { kR, kG, kB, kA, kZero, kOne };
using ComponentMapping = std::array<Swizzle, 4>;
inline constexpr ComponentMapping kIdentityMapping{Swizzle::kR, Swizzle::kG, Swizzle::kB, Swizzle::kA};
// The filters and the address modes of a sampler (the Xenos fetch constant's,
// textures/textures.cpp).
enum class Filter : uint8_t { kPoint, kLinear };
enum class AddressMode : uint8_t { kWrap, kMirror, kClamp, kBorder, kMirrorOnce };
// Their names in the records ("rgba01" per component for a mapping).
inline std::string MappingName(const ComponentMapping& mapping) {
  std::string name;
  for (Swizzle swizzle : mapping) name += "rgba01"[uint32_t(swizzle)];
  return name;
}
constexpr const char* FilterName(Filter filter) { return filter == Filter::kLinear ? "linear" : "point"; }
constexpr const char* AddressModeName(AddressMode mode) {
  switch (mode) {
    case AddressMode::kWrap: return "wrap";
    case AddressMode::kMirror: return "mirror";
    case AddressMode::kClamp: return "clamp";
    case AddressMode::kBorder: return "border";
    case AddressMode::kMirrorOnce: return "mirror-once";
  }
  return "unknown";
}

// One sampled 2D texture of a draw. The engine keeps one own copy
// per key and refreshes it when the layout or the version changes; empty data
// means the caller knows the copy is current. The texture layer uploads
// a copy with UploadSampled and the draw carries no data; versions grow, and
// a draw takes the copy at its version or a later one. A resolve destination
// is sampled from the own texture of its header. The descriptor indices of the
// sampler register are written into the shared constants (+4r view, +192+4r
// sampler).
struct DrawTexture {
  uint32_t reg = 0, key = 0;
  bool resolved = false;
  bool cube = false;  // A cubemap, six array slices; data face after face (level + face * levels).
  TextureFormat format = TextureFormat::kUnknown;
  uint32_t width = 0, height = 0, levels = 1;
  uint64_t layout = 0, version = 0;
  std::vector<std::vector<uint8_t>> data;  // Level rows packed (blocks for BC formats).
  ComponentMapping component_mapping = kIdentityMapping;
  // The sampler: point or linear per stage, or anisotropic up to max_anisotropy.
  Filter min_filter = Filter::kPoint, mag_filter = Filter::kPoint, mip_filter = Filter::kPoint;
  bool anisotropic = false;
  uint32_t max_anisotropy = 1;
  AddressMode address_u = AddressMode::kWrap, address_v = AddressMode::kWrap, address_w = AddressMode::kWrap;
  float lod_bias = 0, min_lod = 0, max_lod = 0;
  std::array<float, 4> border_color{};  // The color of AddressMode::kBorder.
};
// The input layout of a draw, shared by every draw of the pair (VS,
// declaration): the elements and their stable id (FNV of the elements), part
// of the pipeline key.
using VertexElements = std::shared_ptr<const std::vector<VertexElement>>;

// A shader of a draw or a pipeline: its logical identity (the cache name, with
// the spec suffix of a pixel shader variant, or a name of its own for the
// tests and the START policy), its stage, and the binary for the backend's
// API with the spec constants it is specialized for (the cache's mask and the
// value taken). A DXIL binary is the variant linked at build time with them;
// a SPIR-V one takes them when its pipeline is created.
enum class ShaderStage : uint8_t { kVertex, kPixel };
enum class ShaderFormat : uint8_t { kDxil, kSpirv };
struct ShaderRef {
  std::string key;
  ShaderStage stage = ShaderStage::kVertex;
  ShaderFormat format = ShaderFormat::kDxil;
  std::shared_ptr<const std::vector<uint8_t>> bytes;
  uint32_t spec_mask = 0, spec_value = 0;
  bool Present() const { return bytes && !bytes->empty(); }
};

// The primitives of a draw (the title draws triangles only).
enum class Topology : uint8_t { kTriangleList, kTriangleStrip };
constexpr const char* TopologyName(Topology topology) {
  return topology == Topology::kTriangleStrip ? "triangle-strip" : "triangle-list";
}

struct DrawRequest {
  ShaderRef vs{"", ShaderStage::kVertex}, ps{"", ShaderStage::kPixel};
  std::vector<DrawTexture> textures;
  VertexElements elements;
  uint64_t layout_id = 0;  // 0: the engine hashes the elements (tests, the quad).
  std::vector<VertexStream> streams;
  std::vector<uint8_t> indices;  // Inline indices of the draw (index_key 0).
  bool index32 = false;
  // The index buffer the engine keeps, read from index_offset.
  uint64_t index_key = 0;
  uint32_t index_offset = 0;
  uint32_t index_count = 0;
  Topology topology = Topology::kTriangleList;
  int32_t base_vertex = 0;
  // The constant blocks live in the engine (one 4 KB block each for the
  // VS and the PS). A request
  // carries either the whole block (4096 bytes in vs_constants / ps_constants:
  // the evidence draws, the quad, the START lot) or only the registers written
  // since the last request (vs_constants_delta / ps_constants_delta: records of
  // uint16 first register, uint16 count, count x 16 bytes), applied in order on
  // the owner. Empty both: nothing changed.
  ConstantBytes vs_constants, ps_constants;
  ConstantBytes vs_constants_delta, ps_constants_delta;
  // Version of each constant block (0: always uploaded); the engine
  // rebinds the CBV of the previous draw of the batch when the version holds.
  uint64_t vs_constants_version = 0, ps_constants_version = 0;
  std::array<uint8_t, 512> shared_constants{};
  // A posted draw of the batch needs no evidence string; the engine
  // formats it only on failure. Evidence draws (Run) and the tests keep it.
  bool want_evidence = true;
  // The take of the pipeline (batched production draws only; the evidence
  // draws, the quad and the START policy keep the synchronous take). kPrefetch:
  // the owner posts the absent pipelines of the batch to the asynchronous
  // threads before drawing it (Engine::PrefetchPipelines), and the draw takes a
  // ready one, waits for one being created or creates it, never skipped.
  // kSkip (REX_NATIVE_PSO_ASYNC=1, diagnostics): the take before 2026-10-17, a
  // pipeline not ready is posted and this draw records everything but itself,
  // unless its triple (VS, PS, layout) is warm inside the frame's budget
  // (PipelineSet::TakeSkip).
  enum class PipelineTake : uint8_t { kSync, kPrefetch, kSkip };
  PipelineTake pipeline_take = PipelineTake::kSync;
  uint32_t color_key = 0, depth_key = 0;
  // No pixel shader (the title's RB_MODECONTROL depth only): ps without a
  // binary (its key "depth-only"), no color written; color_key may be 0
  // (depth target only).
  bool depth_only = false;
  uint32_t blend_control = 0x00010001, color_mask = 0xF;
  bool alpha_to_coverage = false;  // RB_COLORCONTROL alpha-to-mask
  std::array<float, 4> blend_factor{};
  bool depth_enable = false, depth_write = false;
  uint32_t depth_func = 7;  // Xenos: 0 never .. 7 always.
  // Xenos stencil faces: function | fail << 3 | zpass << 6 | zfail << 9 (bits 8..19
  // or 20..31 of RB_DEPTHCONTROL); ref and masks of RB_STENCILREFMASK.
  bool stencil_enable = false;
  uint32_t stencil_front = 7, stencil_back = 7, stencil_ref = 0, stencil_read_mask = 0xFF, stencil_write_mask = 0xFF;
  bool cull_front = false, cull_back = false, front_ccw = true;
  // Polygon offset as a rasterizer depth bias in the D3D10 integer unit
  // (Xenia's GetD3D10IntegerPolygonOffset and the subpixel slope unit).
  int32_t depth_bias = 0;
  float depth_bias_slope = 0;
  std::array<float, 6> viewport{};  // x, y, width, height, min z, max z
  std::array<int32_t, 4> scissor{};  // left, top, right, bottom
};

// One pipeline of the precompiled set, by the stable identities of
// its key (PipelineList): its shaders (no PS binary when the ps key is
// "depth-only"), the input layout and its id, and the state words of
// DrawIndexed (blend, color mask, depth, cull, target samples and depth and
// color presence, topology class, alpha to coverage, stencil, bias).
struct PipelineDesc {
  ShaderRef vs{"", ShaderStage::kVertex}, ps{"", ShaderStage::kPixel};
  VertexElements elements;
  uint64_t layout_id = 0;
  std::array<uint32_t, 21> state{};
};

// How the swapchain presents (gpu/settings.h). With VSync the Present
// waits for the vblank (interval 1); without it the Present returns at once and
// tears only when tearing is allowed (VRR in borderless fullscreen). The title's
// time does not depend on it: the Swap limiter paces the frames.
struct PresentMode {
  bool vsync = true, allow_tearing = true;
  uint32_t buffers = 2, max_frame_latency = 1;
  // The brightness on the title's ramp (0.5 leaves it).
  float brightness = 0.5f;
  // The output effect of the SDK's presenter and its FXAA at the Swap.
  PresentEffect effect = PresentEffect::kBilinear;
  SwapPostEffect post_effect = SwapPostEffect::kNone;
  float cas_sharpness = 0.0f, fsr_sharpness_reduction = 0.2f;
  uint32_t fsr_passes = 4;
};

// The UI layer of the Present (backend/ui_pass.hlsl): textured triangles drawn
// over the image and its bars, in a coordinate space stretched over the whole
// back buffer, with straight alpha (the contract of rex::ui::ImmediateDrawer).
// Texture 0 is white; the others are the UI textures of UploadUiTexture. A
// draw's indices are relative to its vertex_offset; its scissor rect is in the
// coordinate space.
struct UiVertex {
  float x = 0, y = 0, u = 0, v = 0;
  uint32_t color = 0xFFFFFFFF;  // R8G8B8A8: red in the low byte.
};
struct UiDraw {
  uint32_t texture = 0;
  uint32_t index_offset = 0, index_count = 0, vertex_offset = 0;
  bool scissor = false;
  float scissor_left = 0, scissor_top = 0, scissor_right = 0, scissor_bottom = 0;
};
struct UiFrame {
  float width = 0, height = 0;  // The coordinate space; 0 or no draws draws nothing.
  std::vector<UiVertex> vertices;
  std::vector<uint16_t> indices;
  std::vector<UiDraw> draws;
};
// The scissor rect of a draw in back buffer pixels [left, right) x [top,
// bottom), the whole buffer without one: a pixel is in when its center is, as
// rex::ui::ImmediateDrawer::ScissorToRenderTarget rounds (16.8 fixed point).
// False when the rect holds no pixel.
inline bool UiScissorPixels(const UiDraw& d, const UiFrame& f, uint32_t width, uint32_t height,
                            std::array<uint32_t, 4>& rect) {
  rect = {0, 0, width, height};
  if (!d.scissor) return width && height;
  const auto pixel = [](float value, float scale, uint32_t limit) {
    const float p = std::clamp(value * scale, 0.0f, float(limit));  // Finite: SetUiFrame checks it.
    return uint32_t((int32_t(std::nearbyint(p * 256.0f)) + 127) >> 8);  // Nearest even, as the SDK.
  };
  const float sx = float(width) / f.width, sy = float(height) / f.height;
  rect = {pixel(d.scissor_left, sx, width), pixel(d.scissor_top, sy, height), pixel(d.scissor_right, sx, width),
          pixel(d.scissor_bottom, sy, height)};
  return rect[2] > rect[0] && rect[3] > rect[1];
}

// The host graphics API of the engine, fixed for its life (backend.h).
enum class GraphicsApi : uint8_t { kD3D12, kVulkan };
// The binary format of the shaders the backend of an API takes.
constexpr ShaderFormat ShaderFormatOf(GraphicsApi api) {
  return api == GraphicsApi::kVulkan ? ShaderFormat::kSpirv : ShaderFormat::kDxil;
}

// How the engine renders the title's targets, fixed for its life:
// the internal resolution scale, and the sample count of the targets the title
// creates with MSAA (0 keeps the title's; 2, 4 or 8, lowered at Initialize to
// what the device supports). Single-sample targets stay single-sample. The
// device: the adapter by its DXGI description; empty, or a name not found,
// keeps Plume's choice (the most dedicated video memory). The API: the backend
// the engine selects at Initialize.
struct RenderMode {
  ResolutionScale scale;
  uint32_t msaa_samples = 0;
  std::string device;
#if defined(NARUTORISE_NATIVE_D3D12_DISABLED)
  GraphicsApi api = GraphicsApi::kVulkan;
#else
  GraphicsApi api = GraphicsApi::kD3D12;
#endif
  uint32_t Samples(uint32_t guest) const { return guest > 1 && msaa_samples ? msaa_samples : guest; }
  // A surface or resolve destination in guest pixels and samples, as the engine creates it.
  SurfaceDesc Host(SurfaceDesc desc) const {
    desc.width = scale.Width(desc.width);
    desc.height = scale.Height(desc.height);
    desc.samples = Samples(desc.samples);
    return desc;
  }
};

// The outcome of a backend operation that the common code schedules (the
// creation of a pipeline): success, or the reason and the API's own code
// (an HRESULT, a VkResult), kept as it came.
struct NativeResult {
  bool ok = true;
  const char* reason = nullptr;
  int64_t code = 0;
  static NativeResult Failed(const char* reason, int64_t code) { return {false, reason, code}; }
};
}  // namespace gpu
