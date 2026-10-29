// Host test of the own depth resolve pass, on a real D3D12 device with the
// debug layer. Never enters a guest function or the SDK GPU. A 4X and a 1X
// depth surface are cleared (whole surface, then a rectangle) and resolved
// into R32_FLOAT destinations by the full-screen pass: one sample, the minimum
// of all samples and the single-sample copy. Every texel read back must be the
// rectangle value inside it and the surface value outside. A rectangle, a
// destination of another size, a sample outside the source and an MSAA color
// region are refused. Usage:
// native_engine_depth_resolve_test <new output.jsonl>
#include "gpu/backend/engine.h"
#include "gpu/backend/tests/test_backend.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace gpu;

namespace {
bool Has(const std::string& evidence, const char* text) { return evidence.find(text) != std::string::npos; }
long long Count(const std::string& counts, const char* name) {
  const std::string key = std::string("\"") + name + "\":";
  const size_t at = counts.find(key);
  return at == std::string::npos ? -1 : std::strtoll(counts.c_str() + at + key.size(), nullptr, 10);
}
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

  SurfaceDesc depth4, depth1, color4;
  depth4.key = 0x4400;
  depth4.width = depth4.height = kSize;
  depth4.samples = 4;
  depth4.format = SurfaceFormat::DepthStencil;
  depth1 = depth4;
  depth1.key = 0x4500;
  depth1.samples = 1;
  color4.key = 0x4600;
  color4.width = color4.height = kSize;
  color4.samples = 4;
  line("surface-depth-4x", engine.CreateSurface(depth4, evidence), evidence);
  line("surface-depth-1x", engine.CreateSurface(depth1, evidence), evidence);
  line("surface-color-4x", engine.CreateSurface(color4, evidence), evidence);

  // Whole surface, then the rectangle [kLeft, kRight) x [kTop, kBottom).
  uint64_t fence = 0;
  auto clear = [&](uint32_t key, float outside, float inside) {
    ClearRequest request;
    request.depth_key = key;
    request.clear_depth = request.clear_stencil = true;
    request.depth = outside;
    bool ok = engine.ClearSurfaces(request, fence, evidence);
    request.depth = inside;
    request.left = kLeft; request.top = kTop; request.right = kRight; request.bottom = kBottom;
    return ok && engine.ClearSurfaces(request, fence, evidence);
  };
  line("clear-depth-4x", clear(depth4.key, 0.75f, 0.25f), evidence);
  line("clear-depth-1x", clear(depth1.key, 0.5f, 0.125f), evidence);

  auto resolve = [&](uint32_t source, uint32_t dest, int32_t sample, uint32_t size = kSize) {
    ResolveRequest request;
    request.source_key = source;
    request.dest_key = dest;
    request.dest_width = request.dest_height = size;
    request.dest_format = 23;  // k_24_8_FLOAT, as the START depth destination.
    request.sample = sample;
    return engine.ResolveSurface(request, fence, evidence);
  };
  // Every texel: the rectangle value inside, the surface value outside.
  auto check = [&](uint32_t key, float outside, float inside) {
    std::vector<uint8_t> bytes;
    uint32_t width = 0, height = 0;
    if (!engine.ReadTexture(key, bytes, width, height, evidence) || width != kSize || height != kSize) return false;
    uint32_t wrong = 0;
    for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width; ++x) {
        float value = 0;
        std::memcpy(&value, &bytes[(size_t(y) * width + x) * 4], 4);
        const bool in = x >= kLeft && x < kRight && y >= kTop && y < kBottom;
        wrong += value != (in ? inside : outside);
      }
    evidence.insert(evidence.size() - 1, ",\"wrong_texels\":" + std::to_string(wrong));
    return wrong == 0;
  };
  line("resolve-4x-sample0", resolve(depth4.key, 0x7000, 0) && Has(evidence, "\"method\":\"depth-pass-sample\""),
       evidence);
  line("resolve-4x-min", resolve(depth4.key, 0x7100, -1) && Has(evidence, "\"method\":\"depth-pass-min\""), evidence);
  line("resolve-1x-copy", resolve(depth1.key, 0x7200, -1) && Has(evidence, "\"method\":\"depth-pass-copy\""), evidence);
  line("read-4x-sample0", check(0x7000, 0.75f, 0.25f), evidence);
  line("read-4x-min", check(0x7100, 0.75f, 0.25f), evidence);
  line("read-1x-copy", check(0x7200, 0.5f, 0.125f), evidence);
  // The same destination again after new content: the pass rewrites it.
  line("clear-depth-4x-again", clear(depth4.key, 1.0f, 0.0f), evidence);
  line("resolve-4x-sample3", resolve(depth4.key, 0x7000, 3) && Has(evidence, "\"method\":\"depth-pass-sample\""),
       evidence);
  line("read-4x-sample3", check(0x7000, 1.0f, 0.0f), evidence);

  // Refused: what was deferred with undefined content before.
  ResolveRequest region;
  region.source_key = depth4.key;
  region.dest_key = 0x7300;
  region.dest_width = region.dest_height = kSize;
  region.has_rect = true;
  region.right = region.bottom = 32;
  line("depth-rect-refused", !engine.ResolveSurface(region, fence, evidence) &&
       Has(evidence, "native-depth-resolve-region-uncovered"), evidence);
  line("depth-size-refused", !resolve(depth4.key, 0x7400, 0, 32) &&
       Has(evidence, "native-depth-resolve-region-uncovered"), evidence);
  line("depth-sample-refused", !resolve(depth4.key, 0x7500, 4) &&
       Has(evidence, "native-depth-resolve-sample-uncovered"), evidence);
  ClearRequest color_clear;
  color_clear.color_keys[0] = color4.key;
  color_clear.clear_color = true;
  color_clear.color = {0.25f, 0.5f, 0.75f, 1};
  line("clear-color-4x", engine.ClearSurfaces(color_clear, fence, evidence), evidence);
  region.source_key = color4.key;
  region.dest_key = 0x7600;
  line("msaa-color-rect-refused", !engine.ResolveSurface(region, fence, evidence) &&
       Has(evidence, "native-resolve-msaa-region-uncovered"), evidence);

  line("wait-idle", engine.WaitIdle(evidence), evidence);
  const std::string counts = engine.ResourceCounts();
  line("counts", Count(counts, "depth_resolves") == 4, counts);
  line("release-remaining", engine.ReleaseRemaining(evidence), evidence);
  const bool shutdown = engine.Shutdown(evidence);
  line("shutdown", shutdown && (engine.Api() == GraphicsApi::kVulkan ||
       (Has(evidence, "\"unexpected_debug_messages\":0") && Has(evidence, "\"device_removed_reason\":0"))), evidence);
  output << "{\"kind\":\"verdict\",\"passed\":" << (passed ? "true" : "false") << ",\"guest_executed\":false}\n";
  return passed && output.good() ? 0 : 1;
}
