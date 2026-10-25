// Exercise runtime changes on the same device/window. Resolved guest textures
// survive target rebuilds; new clears/resolves use the new dimensions/samples.
#include "gpu/backend/engine.h"
#include "gpu/backend/tests/test_backend.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <limits>

using namespace gpu;
int main(int argc, char** argv) {
  if (argc != 2 || std::filesystem::exists(argv[1])) return 2;
  std::ofstream output(argv[1]);
  bool passed = true;
  auto check = [&](const char* kind, bool ok, const std::string& detail = "{}") {
    passed &= ok;
    output << "{\"kind\":\"" << kind << "\",\"ok\":" << (ok ? "true" : "false")
           << ",\"evidence\":" << (detail.empty() ? "null" : detail) << "}\n";
    output.flush(); return ok;
  };
  WNDCLASSW cls{}; cls.lpfnWndProc = DefWindowProcW; cls.hInstance = GetModuleHandleW(nullptr);
  cls.lpszClassName = L"LiveRendererSettingsTest"; RegisterClassW(&cls);
  HWND window = CreateWindowW(cls.lpszClassName, L"Live settings test", WS_POPUP, 0, 0, 128, 72,
      nullptr, nullptr, cls.hInstance, nullptr);
  Engine engine; std::string evidence;
  auto mode = TestRenderMode();
  if (!check("initialize", engine.Initialize(evidence, mode), evidence)) return 3;
  PresentMode present; present.vsync = false;
  if (!check("attach", engine.AttachWindow(platform::WindowHandles(window), evidence, present), evidence)) return 4;
  SurfaceDesc color{0x1000, 64, 36, 0, 4}, depth = color;
  depth.key = 0x2000; depth.format = SurfaceFormat::DepthStencil;
  check("color", engine.CreateSurface(color, evidence), evidence);
  check("depth", engine.CreateSurface(depth, evidence), evidence);
  uint64_t fence = 0;
  auto paint = [&](float green) {
    ClearRequest clear; clear.color_keys[0] = color.key; clear.depth_key = depth.key;
    clear.clear_color = clear.clear_depth = clear.clear_stencil = true;
    clear.color = {0.25f, green, 0.75f, 1}; clear.depth = 0.5f; clear.stencil = 71;
    return engine.ClearSurfaces(clear, fence, evidence);
  };
  ResolveRequest resolve; resolve.source_key = color.key; resolve.dest_key = 0x3000;
  resolve.dest_width = 64; resolve.dest_height = 36; resolve.dest_format = 6;
  check("initial-clear", paint(0.5f), evidence);
  check("initial-resolve", engine.ResolveSurface(resolve, fence, evidence), evidence);
  std::vector<uint8_t> retained; uint32_t w = 0, h = 0;
  check("initial-read", engine.ReadTexture(resolve.dest_key, retained, w, h, evidence), evidence);
  unsigned iteration = 0;
  for (const auto& change : {std::pair{1.5f, 2u}, {2.0f, 4u}, {1.0f, 0u}, {1.5f, 2u}, {1.0f, 0u}}) {
    mode.scale = {change.first, change.first}; mode.msaa_samples = change.second;
    present.vsync = !present.vsync; present.buffers = present.buffers == 2 ? 3 : 2;
    present.max_frame_latency = iteration % 3 + 1;
    present.brightness = iteration % 2 ? 0.5f : 0.75f;
    present.effect = iteration % 2 ? PresentEffect::kFsr : PresentEffect::kCas;
    present.post_effect = iteration % 2 ? SwapPostEffect::kNone : SwapPostEffect::kFxaa;
    if (!check("reconfigure", engine.Reconfigure(mode, present, evidence), evidence)) break;
    std::vector<uint8_t> before;
    check("retained-read", engine.ReadTexture(resolve.dest_key, before, w, h, evidence), evidence);
    check("resolved-content-survives", before == retained);
    check("new-clear", paint(0.5f), evidence);
    check("new-resolve", engine.ResolveSurface(resolve, fence, evidence), evidence);
    check("new-read", engine.ReadTexture(resolve.dest_key, retained, w, h, evidence), evidence);
    bool pixels = w == mode.scale.Width(64) && h == mode.scale.Height(36) && retained.size() == size_t(w) * h * 4;
    if (pixels) for (size_t i = 0; i < retained.size(); i += 4)
      pixels &= retained[i] == 64 && retained[i+1] == 128 && retained[i+2] == 191 && retained[i+3] == 255;
    check("new-resolution-and-msaa-pixels", pixels);
    uint32_t slot = 0; bool presented = false; std::vector<uint8_t> frame;
    check("stage", engine.StageFront(resolve.dest_key, slot, evidence), evidence);
    check("present", engine.Present(slot, true, frame, w, h, evidence, &presented), evidence);
    check("presented", presented && w == 128 && h == 72 && !frame.empty());
    auto invalid = mode; invalid.scale.x = std::numeric_limits<float>::quiet_NaN();
    check("invalid-refused", !engine.Reconfigure(invalid, present, evidence), evidence);
    check("still-draws-after-refusal", paint(0.5f), evidence);
    check("still-resolves-after-refusal", engine.ResolveSurface(resolve, fence, evidence), evidence);
    ++iteration;
  }
  check("all-transitions", iteration == 5);
  check("release", engine.ReleaseRemaining(evidence), evidence);
  check("detach", engine.DetachWindow(evidence), evidence);
  check("shutdown", engine.Shutdown(evidence), evidence);
  DestroyWindow(window);
  return passed ? 0 : 1;
}
