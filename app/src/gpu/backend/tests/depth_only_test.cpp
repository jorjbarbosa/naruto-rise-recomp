// Host test of the own depth-only draw (pipelines without a pixel shader),
// on a real D3D12 device with the debug layer.
// Never enters a guest function or the SDK GPU. The VS is the full-screen
// triangle of the depth resolve pass (z = 0, no resources). A 4X depth surface
// without a color target and a 1X depth surface with a cleared color target
// bound are drawn inside a scissor rectangle with LESS and depth write; the
// depth resolve pass reads them back: 0 inside the rectangle, the clear value
// outside. The color target must keep its clear color in every texel. A PS in
// a depth-only draw, a depth-only draw without depth and a normal draw without
// PS are refused. A pipeline precompiled on another thread from the
// identities of a created one is taken by the draw that needs it.
// Usage: native_engine_depth_only_test <new output.jsonl>
#include "gpu/backend/engine.h"
#include "gpu/backend/tests/test_backend.h"
#include "gpu/backend/depth_resolve.h"
#include "gpu/shaders/package.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace gpu;

namespace {
bool Has(const std::string& evidence, const char* text) { return evidence.find(text) != std::string::npos; }
constexpr uint32_t kSize = 64, kLeft = 16, kTop = 8, kRight = 48, kBottom = 40;
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 || std::filesystem::exists(argv[1])) return 2;
  // The debug layer is off by default (engine.cpp); the host test asks for it.
  if (!std::getenv("REX_NATIVE_D3D12_DEBUG")) SetTestEnvironment("REX_NATIVE_D3D12_DEBUG", "1");
  std::ofstream output(argv[1]);
  if (!output) return 3;
  bool passed = true;
  auto line = [&](const char* kind, bool ok, const std::string& evidence) {
    passed = passed && ok;
    output << "{\"kind\":\"" << kind << "\",\"ok\":" << (ok ? "true" : "false") << ",\"evidence\":"
           << (evidence.empty() ? "null" : evidence) << "}\n";
  };
  Engine engine;
  std::string evidence;
  const bool ready = engine.Initialize(evidence, TestRenderMode());
  line("initialize", ready, evidence);
  if (!ready) { engine.Shutdown(evidence); return 4; }

  SurfaceDesc depth4, depth1, color1;
  depth4.key = 0x4400;
  depth4.width = depth4.height = kSize;
  depth4.samples = 4;
  depth4.format = SurfaceFormat::DepthStencil;
  depth1 = depth4;
  depth1.key = 0x4500;
  depth1.samples = 1;
  color1.key = 0x4600;
  color1.width = color1.height = kSize;
  line("surface-depth-4x", engine.CreateSurface(depth4, evidence), evidence);
  line("surface-depth-1x", engine.CreateSurface(depth1, evidence), evidence);
  line("surface-color-1x", engine.CreateSurface(color1, evidence), evidence);

  uint64_t fence = 0;
  auto clear_depth = [&](uint32_t key, float value) {
    ClearRequest request;
    request.depth_key = key;
    request.clear_depth = request.clear_stencil = true;
    request.depth = value;
    return engine.ClearSurfaces(request, fence, evidence);
  };
  line("clear-depth-4x", clear_depth(depth4.key, 0.75f), evidence);
  line("clear-depth-1x", clear_depth(depth1.key, 0.5f), evidence);
  ClearRequest color_clear;
  color_clear.color_keys[0] = color1.key;
  color_clear.clear_color = true;
  color_clear.color = {0.25f, 0.5f, 0.75f, 1};
  line("clear-color-1x", engine.ClearSurfaces(color_clear, fence, evidence), evidence);

  // The full-screen triangle (SV_VertexID 0..2) at z = 0; the POSITION stream
  // only satisfies the input layout.
  auto vs = std::make_shared<const std::vector<uint8_t>>(
      kDepthResolveVs, kDepthResolveVs + sizeof(kDepthResolveVs));
  if (engine.Api() == GraphicsApi::kVulkan) {
    std::string error;
    const auto* package = EmbeddedShaderPackage(ShaderFormat::kSpirv, error);
    if (!package || !package->passes.count("depth_resolve_vs")) return 5;
    vs = std::make_shared<const std::vector<uint8_t>>(DecodeShaderModule(*package, package->passes.at("depth_resolve_vs")));
  }
  auto draw_request = [&](uint32_t depth_key, uint32_t color_key) {
    DrawRequest r;
    r.vs.key = "depth-only-test-vs";
    r.ps.key = "depth-only";
    r.vs.bytes = vs;
    r.vs.format = ShaderFormatOf(engine.Api());
    r.depth_only = true;
    r.elements = std::make_shared<const std::vector<VertexElement>>(
        std::vector<VertexElement>{{"POSITION", 0, VertexFormat::kR32G32B32Float, 0, 0}});
    r.streams.push_back({std::vector<uint8_t>(36), 12});
    r.indices = {0, 0, 1, 0, 2, 0};
    r.index_count = 3;
    r.topology = Topology::kTriangleList;
    r.depth_key = depth_key;
    r.color_key = color_key;
    r.depth_enable = r.depth_write = true;
    r.depth_func = 1;  // Xenos LESS
    r.viewport = {0, 0, float(kSize), float(kSize), 0, 1};
    r.scissor = {int32_t(kLeft), int32_t(kTop), int32_t(kRight), int32_t(kBottom)};
    return r;
  };
  line("draw-depth-only-4x-no-color",
       engine.DrawIndexed(draw_request(depth4.key, 0), fence, evidence) && Has(evidence, "\"drawn\":true") &&
       Has(evidence, "\"samples\":4"), evidence);
  line("draw-depth-only-1x-color-bound",
       engine.DrawIndexed(draw_request(depth1.key, color1.key), fence, evidence) && Has(evidence, "\"drawn\":true"),
       evidence);

  // The identities of the first pipeline with LEQUAL (state word 4)
  // precompiled on another thread, then taken by the draw with that state; the
  // depth stays 0 inside the rectangle, so the reads below do not change.
  {
    const auto descs = engine.PipelineDescs();
    line("prepare-pipelines", descs.size() == 2 && engine.PreparePipelines(evidence), evidence);
    PipelineDesc desc = descs.empty() ? PipelineDesc{} : descs.front();
    desc.vs.bytes = vs;
    desc.state[4] = 3;  // Xenos LEQUAL
    bool present = true, precompiled = false;
    std::string pool;
    std::thread([&] { precompiled = engine.PrecompilePipeline(desc, present, pool); }).join();
    line("precompile-on-another-thread", precompiled && !present, pool);
    std::thread([&] { precompiled = engine.PrecompilePipeline(desc, present, pool); }).join();
    line("precompile-again-present", precompiled && present, pool);
    auto lequal = draw_request(depth4.key, 0);
    lequal.depth_func = 3;
    line("draw-takes-precompiled", engine.DrawIndexed(lequal, fence, evidence) && Has(evidence, "\"drawn\":true") &&
         Has(evidence, "\"pipeline_created\":true"), evidence);
    const std::string list = engine.PipelineList();
    auto count = [&](const char* text) {
      size_t n = 0;
      for (size_t at = list.find(text); at != std::string::npos; at = list.find(text, at + 1)) ++n;
      return n;
    };
    const size_t created = count("\"origin\":\"created\""), taken = count("\"origin\":\"precompiled\"");
    line("pipeline-list-origins", created == 2 && taken == 1,
         "{\"created\":" + std::to_string(created) + ",\"precompiled\":" + std::to_string(taken) + "}");
  }

  auto resolve = [&](uint32_t source, uint32_t dest, int32_t sample) {
    ResolveRequest request;
    request.source_key = source;
    request.dest_key = dest;
    request.dest_width = request.dest_height = kSize;
    request.dest_format = 23;  // k_24_8_FLOAT for depth; a color destination is RGBA8.
    request.sample = sample;
    return engine.ResolveSurface(request, fence, evidence);
  };
  // Every depth texel: 0 inside the scissor rectangle, the clear value outside.
  auto check_depth = [&](uint32_t key, float outside) {
    std::vector<uint8_t> bytes;
    uint32_t width = 0, height = 0;
    if (!engine.ReadTexture(key, bytes, width, height, evidence) || width != kSize || height != kSize) return false;
    uint32_t wrong = 0;
    for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width; ++x) {
        float value = 0;
        std::memcpy(&value, &bytes[(size_t(y) * width + x) * 4], 4);
        const bool in = x >= kLeft && x < kRight && y >= kTop && y < kBottom;
        wrong += value != (in ? 0.0f : outside);
      }
    evidence.insert(evidence.size() - 1, ",\"wrong_texels\":" + std::to_string(wrong));
    return wrong == 0;
  };
  line("resolve-depth-4x-sample0", resolve(depth4.key, 0x7000, 0), evidence);
  line("resolve-depth-4x-min", resolve(depth4.key, 0x7100, -1), evidence);
  line("resolve-depth-1x", resolve(depth1.key, 0x7200, -1), evidence);
  line("read-depth-4x-sample0", check_depth(0x7000, 0.75f), evidence);
  line("read-depth-4x-min", check_depth(0x7100, 0.75f), evidence);
  line("read-depth-1x", check_depth(0x7200, 0.5f), evidence);
  // The color target bound during the 1X depth-only draw keeps its clear color.
  line("resolve-color-1x", resolve(color1.key, 0x7300, -1), evidence);
  {
    std::vector<uint8_t> rgba;
    uint32_t width = 0, height = 0;
    bool ok = engine.ReadTexture(0x7300, rgba, width, height, evidence) && width == kSize && height == kSize;
    uint32_t differing = 0;
    const uint8_t first[4] = {ok ? rgba[0] : uint8_t(0), ok ? rgba[1] : uint8_t(0), ok ? rgba[2] : uint8_t(0),
                              ok ? rgba[3] : uint8_t(0)};
    for (size_t i = 0; ok && i + 4 <= rgba.size(); i += 4) differing += std::memcmp(&rgba[i], first, 4) != 0;
    auto near = [](uint8_t value, int expected) { return std::abs(int(value) - expected) <= 1; };
    ok = ok && differing == 0 && near(first[0], 64) && near(first[1], 128) && near(first[2], 191) && first[3] == 255;
    evidence.insert(evidence.size() - 1, ",\"differing_texels\":" + std::to_string(differing) + ",\"texel\":[" +
                    std::to_string(first[0]) + "," + std::to_string(first[1]) + "," + std::to_string(first[2]) + "," +
                    std::to_string(first[3]) + "]");
    line("read-color-1x-unchanged", ok, evidence);
  }

  // Refused: a PS in a depth-only draw, a depth-only draw without depth and a
  // normal draw without PS.
  auto with_ps = draw_request(depth4.key, 0);
  with_ps.ps.bytes = vs;
  line("depth-only-with-ps-refused", !engine.DrawIndexed(with_ps, fence, evidence) &&
       Has(evidence, "native-draw-shaders"), evidence);
  auto without_depth = draw_request(depth4.key, 0);
  without_depth.depth_enable = without_depth.depth_write = false;
  line("depth-only-without-depth-refused", !engine.DrawIndexed(without_depth, fence, evidence) &&
       Has(evidence, "native-draw-depth-only-without-depth"), evidence);
  auto normal = draw_request(depth1.key, color1.key);
  normal.depth_only = false;
  line("normal-without-ps-refused", !engine.DrawIndexed(normal, fence, evidence) &&
       Has(evidence, "native-draw-shaders"), evidence);

  line("wait-idle", engine.WaitIdle(evidence), evidence);
  line("release-remaining", engine.ReleaseRemaining(evidence), evidence);
  const bool shutdown = engine.Shutdown(evidence);
  line("shutdown", shutdown && (engine.Api() == GraphicsApi::kVulkan ||
       (Has(evidence, "\"unexpected_debug_messages\":0") && Has(evidence, "\"device_removed_reason\":0"))), evidence);
  output << "{\"kind\":\"verdict\",\"passed\":" << (passed ? "true" : "false") << ",\"guest_executed\":false}\n";
  return passed && output.good() ? 0 : 1;
}
