// Host test of the own resource lifetime, on a real D3D12 device.
// Never enters a guest function or the SDK GPU. A resolve destination
// recreated while its last use is in flight retires the old texture until the
// fence, and is recreated in place once idle; released in flight it is retired
// and freed by the fence. A sampled copy
// (seeded as a draw leaves it) released in flight is retired with its SRV slot,
// released idle frees the slot at once, and both are gone after WaitIdle;
// ReleaseRemaining leaves no sampled copy. Usage:
// native_engine_lifetime_test <new output.jsonl>
#include "gpu/backend/engine.h"
#include "gpu/backend/tests/test_backend.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace gpu;

namespace {
bool Has(const std::string& evidence, const char* text) { return evidence.find(text) != std::string::npos; }
// The integer after "name": in a counts object; -1 when absent.
long long Count(const std::string& counts, const char* name) {
  const std::string key = std::string("\"") + name + "\":";
  const size_t at = counts.find(key);
  return at == std::string::npos ? -1 : std::strtoll(counts.c_str() + at + key.size(), nullptr, 10);
}
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
  SurfaceDesc source;
  source.key = 0x4200;
  source.width = source.height = 64;
  line("surface", engine.CreateSurface(source, evidence), evidence);
  ClearRequest clear;
  clear.color_keys[0] = source.key;
  clear.clear_color = true;
  clear.color = {0.25f, 0.5f, 0.75f, 1};
  uint64_t fence = 0;
  line("clear", engine.ClearSurfaces(clear, fence, evidence), evidence);

  // Resolve destination: recreated in flight with the old texture retired
  // until its fence, recreated in place once idle.
  ResolveRequest resolve;
  resolve.source_key = source.key;
  resolve.dest_key = 0x5000;
  resolve.dest_width = resolve.dest_height = 64;
  resolve.dest_format = 6;
  line("resolve-created", engine.ResolveSurface(resolve, fence, evidence) && Has(evidence, "\"resolved\":true"), evidence);
  resolve.dest_width = resolve.dest_height = 32;
  line("resolve-recreated-in-flight", engine.ResolveSurface(resolve, fence, evidence) &&
       Has(evidence, "\"resolved\":true") && Has(evidence, "\"dest_size\":[32,32]") &&
       Count(engine.ResourceCounts(), "retired_textures") == 1, evidence);
  line("wait-idle-1", engine.WaitIdle(evidence), evidence);
  line("recreate-retired-freed-by-fence", Count(engine.ResourceCounts(), "retired_textures") == 0,
       engine.ResourceCounts());
  resolve.dest_width = resolve.dest_height = 64;
  line("resolve-recreated-when-idle", engine.ResolveSurface(resolve, fence, evidence) &&
       Has(evidence, "\"resolved\":true") && Has(evidence, "\"dest_size\":[64,64]") &&
       Count(engine.ResourceCounts(), "retired_textures") == 0, evidence);
  line("resolve-destination-retired-in-flight", engine.ReleaseTexture(resolve.dest_key, evidence) &&
       Has(evidence, "\"own_texture\":true") && Has(evidence, "\"deferred\":true") &&
       Count(engine.ResourceCounts(), "retired_textures") == 1, evidence);

  // Sampled copies: retired with the fence in flight, freed at once when idle.
  const std::string before = engine.ResourceCounts();
  line("seed-sampled-in-flight", engine.SeedSampledForTest(0x6000, true, evidence), evidence);
  const long long free_after_seed = Count(engine.ResourceCounts(), "views_free");
  const bool retired = engine.ReleaseSampled(0x6000, evidence) && Has(evidence, "\"sampled\":true") &&
                       Has(evidence, "\"deferred\":true");
  std::string counts = engine.ResourceCounts();
  line("sampled-retired-in-flight", retired && Count(counts, "sampled") == 0 && Count(counts, "retired_sampled") == 1 &&
       Count(counts, "sampled_retired") == 1 && Count(counts, "views_free") == free_after_seed, evidence);
  line("seed-sampled-idle", engine.SeedSampledForTest(0x6100, false, evidence), evidence);
  const long long free_after_idle_seed = Count(engine.ResourceCounts(), "views_free");
  const bool released = engine.ReleaseSampled(0x6100, evidence) && Has(evidence, "\"deferred\":false");
  counts = engine.ResourceCounts();
  line("sampled-released-idle", released && Count(counts, "sampled_released") == 1 &&
       Count(counts, "views_free") == free_after_idle_seed + 1 && Count(counts, "retired_sampled") == 1, evidence);
  line("sampled-unknown-address", engine.ReleaseSampled(0x6200, evidence) && Has(evidence, "\"sampled\":false"),
       evidence);
  line("wait-idle-2", engine.WaitIdle(evidence), evidence);
  counts = engine.ResourceCounts();
  line("retired-freed-by-fence", Count(counts, "retired_sampled") == 0 && Count(counts, "retired_textures") == 0 &&
       Count(counts, "sampled_released") == 2 && Count(counts, "textures_released") >= 1 &&
       Count(counts, "views_free") >= free_after_idle_seed + 2, "{\"before\":" + before + ",\"after\":" + counts + "}");

  // Ordered close: ReleaseRemaining leaves no sampled copy, in flight or not.
  line("clear-2", engine.ClearSurfaces(clear, fence, evidence), evidence);
  line("seed-remaining", engine.SeedSampledForTest(0x6300, false, evidence) &&
       engine.SeedSampledForTest(0x6400, true, evidence), evidence);
  const bool remaining = engine.ReleaseRemaining(evidence);
  counts = engine.ResourceCounts();
  line("release-remaining", remaining && Count(counts, "sampled") == 0 && Count(counts, "retired_sampled") == 0 &&
       Count(counts, "textures") == 0 && Count(counts, "surfaces") == 0 && Count(counts, "retired_textures") == 0 &&
       Count(counts, "sampled_released") == 4, evidence);
  line("shutdown", engine.Shutdown(evidence), evidence);
  output << "{\"kind\":\"verdict\",\"passed\":" << (passed ? "true" : "false") << ",\"guest_executed\":false}\n";
  return passed && output.good() ? 0 : 1;
}
