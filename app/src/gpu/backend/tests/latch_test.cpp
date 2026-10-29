// Host test of a posted failure followed by WaitIdle. Never
// enters a guest function or the SDK GPU. A clear is recorded into the open
// batch, a draw fails and is latched as the dispatcher does for a posted
// operation; then WaitIdle must report false without a new Signal, every
// later operation must be refused, and Shutdown must discard the batch that
// was never submitted. Usage: native_engine_latch_test <new output.jsonl>
#include "gpu/backend/engine.h"
#include "gpu/backend/tests/test_backend.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace gpu;

namespace {
bool Has(const std::string& evidence, const char* text) { return evidence.find(text) != std::string::npos; }
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 || std::filesystem::exists(argv[1])) return 2;
  // The debug layer is off by default (engine.cpp); the host test asks for it.
  if (!std::getenv("REX_NATIVE_D3D12_DEBUG")) SetTestEnvironment("REX_NATIVE_D3D12_DEBUG", "1");
  std::ofstream output(argv[1]);
  if (!output) return 3;
  auto line = [&](const char* kind, bool ok, const std::string& evidence) {
    output << "{\"kind\":\"" << kind << "\",\"ok\":" << (ok ? "true" : "false") << ",\"evidence\":"
           << (evidence.empty() ? "null" : evidence) << "}\n";
  };
  Engine engine;
  std::string evidence;
  const bool ready = engine.Initialize(evidence, TestRenderMode());
  line("initialize", ready, evidence);
  if (!ready) { engine.Shutdown(evidence); return 4; }
  SurfaceDesc desc;
  desc.key = 0x4200;
  desc.width = 64;
  desc.height = 64;
  const bool created = engine.CreateSurface(desc, evidence);
  line("surface", created, evidence);
  ClearRequest clear;
  clear.color_keys[0] = desc.key;
  clear.clear_color = true;
  clear.color = {0, 0, 0, 1};
  uint64_t clear_fence = 0;
  const bool cleared = engine.ClearSurfaces(clear, clear_fence, evidence);
  line("clear-recorded", cleared, evidence);
  DrawRequest bad;  // No color surface: the engine refuses it, as a failed posted draw.
  uint64_t draw_fence = 0;
  const bool drawn = engine.DrawIndexed(bad, draw_fence, evidence);
  line("draw-fails", !drawn, evidence);
  engine.LatchFailure(7, evidence);
  const bool latched = engine.Latched();
  const bool idle = engine.WaitIdle(evidence);
  const bool idle_ok = !idle && Has(evidence, "posted-operation-failed") && Has(evidence, "\"failed_sequence\":7") &&
                       Has(evidence, "\"target\":0") && Has(evidence, "\"batches_closed\":0");
  line("wait-idle-refused-no-signal", latched && idle_ok, evidence);
  const bool second = engine.ClearSurfaces(clear, clear_fence, evidence);
  const bool second_ok = !second && Has(evidence, "posted-operation-failed");
  line("clear-refused", second_ok, evidence);
  std::vector<uint8_t> rgba;
  uint32_t width = 0, height = 0;
  const bool read = engine.ReadSurface(desc.key, clear_fence, rgba, width, height, evidence);
  const bool read_ok = !read && Has(evidence, "posted-operation-failed");
  line("read-refused", read_ok, evidence);
  const bool released = engine.Shutdown(evidence);
  const bool shutdown_ok = released && Has(evidence, "\"open_batch_discarded\":true") &&
                           Has(evidence, "\"batches_closed\":0") && Has(evidence, "\"posted_failure_sequence\":7");
  line("shutdown-discards-open-batch", shutdown_ok, evidence);
  const bool passed = ready && created && cleared && !drawn && latched && idle_ok && second_ok && read_ok && shutdown_ok;
  output << "{\"kind\":\"verdict\",\"passed\":" << (passed ? "true" : "false") << ",\"guest_executed\":false}\n";
  return passed && output.good() ? 0 : 1;
}
