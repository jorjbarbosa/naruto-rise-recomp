// Real GPU readbacks after repeated bindings, changes, shader resolves and
// command-list resets. The shaders and geometry are independent of the game.
#include "gpu/backend/engine.h"
#include "bindings_vs.h"
#include "bindings_ps.h"
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

using namespace gpu;
using Color = std::array<uint8_t, 4>;

int main(int argc, char** argv) {
  if (argc != 2 || std::filesystem::exists(argv[1])) return 2;
  _putenv_s("REX_NATIVE_D3D12_DEBUG", "1");
  std::ofstream output(argv[1]);
  bool passed = true;
  Engine engine;
  std::string e;
  uint64_t fence = 0;
  auto check = [&](const char* name, bool ok) {
    passed &= ok;
    output << "{\"kind\":\"" << name << "\",\"ok\":" << (ok ? "true" : "false")
           << ",\"evidence\":" << (e.empty() ? "null" : e) << "}\n";
    return ok;
  };
  if (!check("initialize", engine.Initialize(e, RenderMode{}))) return 3;
  SurfaceDesc a{0x1000, 64, 64}, b{0x2000, 64, 64}, depth{0x3000, 64, 64};
  depth.format = SurfaceFormat::DepthStencil;
  check("target-a", engine.CreateSurface(a, e));
  check("target-b", engine.CreateSurface(b, e));
  check("depth", engine.CreateSurface(depth, e));
  const float full[]{-1, -1, 0, -1, 3, 0, 3, -1, 0};
  const float right[]{0, -1, 0, 0, 3, 0, 2, -1, 0};
  const uint16_t triangle[]{0, 1, 2}, degenerate[]{0, 0, 0};
  auto upload = [&](uint64_t key, const auto& data) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(data);
    BufferUpload u{key, 1, sizeof(data), {{0, std::vector<uint8_t>(bytes, bytes + sizeof(data))}}};
    check("upload", engine.UploadBuffer(u, e));
  };
  upload(1, full); upload(2, right); upload(3, triangle); upload(4, degenerate);
  DrawRequest r;
  r.vs = {"bindings-vs", ShaderStage::kVertex, ShaderFormat::kDxil,
          std::make_shared<const std::vector<uint8_t>>(kBindings_vs, kBindings_vs + sizeof(kBindings_vs))};
  r.ps = {"bindings-ps", ShaderStage::kPixel, ShaderFormat::kDxil,
          std::make_shared<const std::vector<uint8_t>>(kBindings_ps, kBindings_ps + sizeof(kBindings_ps))};
  r.elements = std::make_shared<const std::vector<VertexElement>>(
      std::vector<VertexElement>{{"POSITION", 0, VertexFormat::kR32G32B32Float, 0, 0}});
  r.streams.push_back({{}, 12, 1, 0, sizeof(full)});
  r.index_key = 3; r.index_count = 3;
  r.color_key = a.key; r.depth_key = depth.key;
  r.depth_enable = true; r.depth_func = 7;
  r.viewport = {0, 0, 64, 64, 0, 1}; r.scissor = {0, 0, 64, 64};
  r.vs_constants.resize(4096); r.ps_constants.resize(4096);
  r.vs_constants_version = r.ps_constants_version = 1;
  const std::array<float, 4> white{1, 1, 1, 1};
  std::memcpy(r.shared_constants.data(), white.data(), 16);
  auto tint = [&](std::array<float, 4> value) {
    std::memcpy(r.ps_constants.data(), value.data(), 16);
    ++r.ps_constants_version;
  };
  auto draw = [&] { check("draw", engine.DrawIndexed(r, fence, e)); };
  auto clear = [&](uint32_t target) {
    ClearRequest c;
    c.color_keys[0] = target; c.clear_color = true; c.color = {0, 0, 0, 1};
    c.depth_key = depth.key; c.clear_depth = c.clear_stencil = true; c.depth = 1;
    check("clear", engine.ClearSurfaces(c, fence, e));
  };
  auto pixels = [&](const char* name, uint32_t key, Color left, Color right_color) {
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    bool ok = engine.ReadSurface(key, fence, rgba, w, h, e) && w == 64 && h == 64;
    if (ok) for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
      const auto& want = x < 32 ? left : right_color;
      ok &= std::memcmp(rgba.data() + (y * w + x) * 4, want.data(), 4) == 0;
    }
    check(name, ok);
  };
  constexpr Color red{255, 0, 0, 255}, green{0, 255, 0, 255}, blue{0, 0, 255, 255};
  constexpr Color yellow{255, 255, 0, 255}, black{0, 0, 0, 255}, white_pixel{255, 255, 255, 255};
  clear(a.key); clear(b.key);
  tint({1, 0, 0, 1});
  for (unsigned i = 0; i < 100; ++i) draw();
  clear(a.key); draw();  // A clear must not invalidate otherwise identical bindings.
  r.scissor = {32, 0, 64, 64}; tint({0, 1, 0, 1}); draw();
  r.scissor = {0, 0, 64, 64}; r.viewport[2] = 32; tint({0, 0, 1, 1}); draw();
  r.color_key = b.key; r.viewport[2] = 64; tint(white);
  const std::array<float, 4> shared_yellow{1, 1, 0, 1};
  std::memcpy(r.shared_constants.data(), shared_yellow.data(), 16); draw();
  r.color_key = a.key; r.viewport[2] = 32; tint({0, 0, 1, 1});
  std::memcpy(r.shared_constants.data(), white.data(), 16); draw();
  // The resolve replaces the root signature, heaps, PSO, viewport and targets.
  // The draw request afterward is identical to the one before it.
  ResolveRequest resolve;
  resolve.source_key = depth.key; resolve.dest_key = 0x4000;
  resolve.dest_width = resolve.dest_height = 64; resolve.dest_format = 23;
  check("shader-resolve", engine.ResolveSurface(resolve, fence, e));
  draw();
  pixels("restore-after-resolve", a.key, blue, green);
  pixels("target-and-shared-constants", b.key, yellow, yellow);
  // Readback closed the command list. Reuse versions and request on the next.
  clear(a.key); draw();
  pixels("restore-after-reset", a.key, blue, black);
  r.viewport[2] = 64; r.streams[0].key = 2;
  r.index_key = 4; tint({1, 0, 0, 1}); draw();
  pixels("changed-degenerate-indices", a.key, blue, black);
  r.index_key = 3; tint(white);
  r.blend_control = 12 | (1 << 16);  // Constant RGB, opaque alpha.
  r.blend_factor = {1, 0, 0, 1}; draw();
  r.blend_factor = {0, 1, 0, 1}; draw();
  pixels("changed-vertex-buffer-and-blend", a.key, blue, green);
  // VS constants move the right-half geometry to the left; PS and shared CBVs
  // stay unchanged. The left becomes green, leaving the right green as well.
  const float shift = -1;
  std::memcpy(r.vs_constants.data(), &shift, 4); ++r.vs_constants_version; draw();
  pixels("changed-vs-constants", a.key, green, green);
  r.streams[0].key = 1; std::memset(r.vs_constants.data(), 0, 16); ++r.vs_constants_version;
  r.blend_control = 0x00010001; r.stencil_enable = true;
  r.stencil_front = r.stencil_back = 7 | (2 << 6);  // ALWAYS / REPLACE on pass.
  r.stencil_ref = 5; r.color_mask = 0; draw();
  r.stencil_front = r.stencil_back = 2;  // EQUAL / KEEP.
  r.color_mask = 15; tint(white); draw();
  r.stencil_ref = 6; tint({0, 0, 1, 1}); draw();  // Must fail the stencil test.
  pixels("changed-stencil-reference", a.key, white_pixel, white_pixel);
  r.stencil_enable = false; r.stencil_ref = 9; tint({1, 0, 0, 1}); draw();
  r.stencil_enable = true; r.stencil_ref = 5; r.scissor = {32, 0, 64, 64};
  tint({0, 0, 1, 1}); draw();
  pixels("stencil-disabled-then-enabled", a.key, red, blue);
  check("release", engine.ReleaseRemaining(e));
  check("shutdown", engine.Shutdown(e));
  output << "{\"kind\":\"verdict\",\"passed\":" << (passed ? "true" : "false") << "}\n";
  return passed && output.good() ? 0 : 1;
}
