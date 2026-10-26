#include "gpu/backend/engine.h"
#include "gpu/backend/tests/test_backend.h"
#include "draw_cache_vs.h"
#include "draw_cache_ps.h"
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iostream>

using namespace gpu;
int main(int argc, char** argv) {
  if (argc != 2 || std::filesystem::exists(argv[1])) return 2;
  std::ofstream output(argv[1]); bool passed = true;
  auto check = [&](const char* kind, bool ok, const std::string& e) {
    passed &= ok;
    output << "{\"kind\":\"" << kind << "\",\"ok\":" << (ok ? "true" : "false")
           << ",\"evidence\":" << (e.empty() ? "null" : e) << "}\n";
  };
  Engine engine; std::string e; uint64_t fence = 0;
  if (!engine.Initialize(e, TestRenderMode())) { check("initialize", false, e); return 3; }
  check("initialize", true, e);
  SurfaceDesc surface{0x1234, 64, 64};
  check("surface", engine.CreateSurface(surface, e), e);
  DrawRequest r;
  r.vs = {"cache-test-vs", ShaderStage::kVertex, ShaderFormat::kSpirv,
      std::make_shared<const std::vector<uint8_t>>(reinterpret_cast<const uint8_t*>(kCacheVs),
          reinterpret_cast<const uint8_t*>(kCacheVs) + sizeof(kCacheVs))};
  r.ps = {"cache-test-ps", ShaderStage::kPixel, ShaderFormat::kSpirv,
      std::make_shared<const std::vector<uint8_t>>(reinterpret_cast<const uint8_t*>(kCachePs),
          reinterpret_cast<const uint8_t*>(kCachePs) + sizeof(kCachePs))};
  r.elements = std::make_shared<const std::vector<VertexElement>>(
      std::vector<VertexElement>{{"POSITION", 0, VertexFormat::kR32G32B32Float, 0, 0}});
  r.streams.push_back({std::vector<uint8_t>(36), 12});
  r.indices = {0, 0, 1, 0, 2, 0}; r.index_count = 3;
  r.color_key = surface.key; r.viewport = {0, 0, 64, 64, 0, 1}; r.scissor = {0, 0, 64, 64};
  r.vs_constants.resize(4096); r.ps_constants.resize(4096);
  const float white[]{1, 1, 1, 1}; std::memcpy(r.ps_constants.data(), white, 16);
  r.vs_constants_version = r.ps_constants_version = 1;
  DrawTexture t; t.reg = 0; t.key = 0x6000; t.format = TextureFormat::kR8G8B8A8Unorm;
  t.width = t.height = 2; t.levels = 1; t.layout = t.version = 1;
  auto upload = [&](std::array<uint8_t, 4> color) {
    t.data.assign(t.cube ? 6 : 1, std::vector<uint8_t>(size_t(t.width) * t.height * 4));
    for (auto& face : t.data) for (size_t at = 0; at < face.size(); at += 4)
      std::memcpy(face.data() + at, color.data(), 4);
    const bool ok = engine.UploadSampled(t, e); t.data.clear(); return ok;
  };
  check("upload-red", upload({255, 0, 0, 255}), e);
  r.textures = {t};
  check("draw-red", engine.DrawIndexed(r, fence, e), e);
  r.vs_constants.clear(); r.ps_constants.clear();
  for (unsigned i = 0; i < 96; ++i) {
    // Switch compatible pipelines inside the pass; RGB-only keeps the alpha
    // already written by the first draw. All other bindings stay the same.
    r.color_mask = i < 4 ? 7 : 15;
    if (!engine.DrawIndexed(r, fence, e)) { check("repeat", false, e); break; }
  }
  r.scissor = {32, 0, 64, 64}; r.textures[0].component_mapping = {Swizzle::kG, Swizzle::kR, Swizzle::kB, Swizzle::kA};
  check("draw-swizzle-green", engine.DrawIndexed(r, fence, e), e);
  // Same image/view, new content: cached descriptors must read the new upload.
  t.version = 2;
  check("upload-blue", upload({0, 0, 255, 255}), e);
  r.scissor = {0, 32, 32, 64}; r.textures = {t};
  check("draw-new-version-blue", engine.DrawIndexed(r, fence, e), e);
  // Recreated image under the same resource key, with the old views in flight.
  t.width = t.height = 4; t.layout = t.version = 3;
  check("recreate-white", upload({255, 255, 255, 255}), e);
  r.scissor = {32, 32, 64, 64}; r.textures = {t};
  r.ps_constants.resize(4096); const float yellow[]{1, 1, 0, 1};
  std::memcpy(r.ps_constants.data(), yellow, 16); r.ps_constants_version = 2;
  check("draw-constant-version-yellow", engine.DrawIndexed(r, fence, e), e);
  check("release-in-flight", engine.ReleaseSampled(t.key, e), e);
  std::vector<uint8_t> pixels; uint32_t width{}, height{};
  bool read = engine.ReadSurface(surface.key, fence, pixels, width, height, e);
  check("read", read, e);
  bool colors = read && width == 64 && height == 64;
  if (colors) for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
    const std::array<uint8_t, 4> expected = y < 32 ?
        (x < 32 ? std::array<uint8_t, 4>{255, 0, 0, 255} : std::array<uint8_t, 4>{0, 255, 0, 255}) :
        (x < 32 ? std::array<uint8_t, 4>{0, 0, 255, 255} : std::array<uint8_t, 4>{255, 255, 0, 255});
    colors &= std::memcmp(pixels.data() + (y * width + x) * 4, expected.data(), 4) == 0;
  }
  check("every-texel", colors, "{}");
  auto summary = engine.PipelineSummary();
  auto number = [&](const char* name) {
    const std::string token = std::string("\"") + name + "\":";
    auto at = summary.find(token);
    return at == std::string::npos ? UINT64_MAX : std::strtoull(summary.c_str() + at + token.size(), nullptr, 10);
  };
  check("cached-bindings", number("vulkan_draws") == 100 && number("draw_descriptor_groups") == 3 &&
      number("draw_samplers") == 2 && number("draw_framebuffers") == 1 && number("draw_image_views") == 4 &&
      number("draw_constant_bytes") == 100 * 512 + 3 * 4096 && number("draw_render_passes") == 3,
      "{" + summary + "}");
  // GPU completion permits slot reuse. Recreate the released texture key and
  // reuse constant versions: no descriptor or BDA from the old batch may
  // survive as a valid binding in this one.
  t.layout = t.version = 4;
  check("second-batch-upload", upload({0, 0, 255, 255}), e);
  r.textures = {t}; r.scissor = {0, 0, 64, 64};
  std::memcpy(r.ps_constants.data(), white, 16); r.ps_constants_version = 2;
  for (unsigned i = 0; i < 21; ++i) {
    if (!engine.DrawIndexed(r, fence, e)) { check("second-batch-draw", false, e); break; }
  }
  read = engine.ReadSurface(surface.key, fence, pixels, width, height, e);
  colors = read && width == 64 && height == 64;
  if (colors) for (size_t at = 0; at < pixels.size(); at += 4) {
    const uint8_t blue[]{0, 0, 255, 255};
    colors &= std::memcmp(pixels.data() + at, blue, 4) == 0;
  }
  check("second-batch-every-texel", colors, e);
  summary = engine.PipelineSummary();
  check("recycled-descriptors", number("draw_descriptor_pools") == 1 && number("draw_set_allocations") == 12 &&
      number("draw_samplers") == 2 && number("vulkan_draws") == 121, "{" + summary + "}");
  check("recycled-write-count", number("draw_descriptor_elements") == 385, "{" + summary + "}");
  auto shader_input = [&](uint32_t reg, uint32_t forced, uint32_t cube) {
    const uint32_t values[]{reg, forced, cube};
    std::memcpy(r.ps_constants.data() + 16, values, sizeof(values));
    ++r.ps_constants_version;
  };
  auto solid = [&](const char* label, std::array<uint8_t, 4> expected) {
    bool ok = engine.DrawIndexed(r, fence, e) && engine.ReadSurface(surface.key, fence, pixels, width, height, e);
    ok &= width == 64 && height == 64 && pixels.size() == 64 * 64 * 4;
    if (ok) for (size_t at = 0; at < pixels.size(); at += 4)
      ok &= std::memcmp(pixels.data() + at, expected.data(), 4) == 0;
    check(label, ok, e);
  };
  // Recycled descriptor storage must clear entries that were active before,
  // even when no current texture references that slot. Explicitly sample it.
  r.textures.clear(); shader_input(0, 1, 0);
  solid("removed-image-slot-zero", {0, 0, 0, 0});
  t.reg = 19; r.textures = {t}; shader_input(19, 0, 0);
  solid("high-register-slot", {0, 0, 255, 255});
  r.textures.clear(); shader_input(19, 20, 0);
  solid("removed-high-slot-zero", {0, 0, 0, 0});
  // Recreate the same resource key as a cube, then remove that cube binding.
  t.reg = 0; t.cube = true; ++t.layout; ++t.version;
  check("upload-cube", upload({0, 255, 0, 255}), e);
  r.textures = {t}; shader_input(0, 0, 1);
  solid("cube-slot", {0, 255, 0, 255});
  r.textures.clear(); shader_input(0, 1, 1);
  solid("removed-cube-slot-zero", {0, 0, 0, 0});
  t.cube = false; ++t.layout; ++t.version;
  check("recreate-2d", upload({255, 0, 0, 255}), e);
  shader_input(0, 0, 0); r.textures = {t};
  // Cross the 64-group pool boundary and then reuse every group. Different
  // samplers force different immutable groups without changing these pixels.
  for (unsigned cycle = 0; cycle < 2; ++cycle) {
    for (unsigned i = 0; i < 70; ++i) {
      r.textures[0].lod_bias = float(i) / 100;
      if (!engine.DrawIndexed(r, fence, e)) { check("many-groups-draw", false, e); break; }
    }
    solid(cycle ? "many-groups-recycled" : "many-groups-in-flight", {255, 0, 0, 255});
  }
  summary = engine.PipelineSummary();
  check("final-write-counters", number("draw_descriptor_groups") > 140 &&
      number("draw_descriptor_elements") < 128 * number("draw_descriptor_groups"), "{" + summary + "}");
  check("release", engine.ReleaseRemaining(e), e);
  check("shutdown", engine.Shutdown(e), e);
  output << "{\"kind\":\"verdict\",\"passed\":" << (passed ? "true" : "false") << "}\n";
  return passed ? 0 : 1;
}
