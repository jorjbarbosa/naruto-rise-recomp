// GPU regression for the scaled UI fringe at presentation. The front surface
// has a red one-guest-pixel top/left edge, green interior and a blue quadrant.
// Presentation must extend the interior over the fringe above 1x, preserve 1x
// and the interior coordinates, and leave the source surface unchanged.
#include "gpu/backend/engine.h"
#include "gpu/backend/tests/test_backend.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace gpu;

int main(int argc, char** argv) {
  if (argc != 2 || std::filesystem::exists(argv[1])) return 2;
  std::ofstream output(argv[1]);
  if (!output) return 3;
  bool passed = true;
  auto check = [&](const char* kind, bool ok, const std::string& evidence) {
    passed &= ok;
    output << "{\"kind\":\"" << kind << "\",\"ok\":" << (ok ? "true" : "false")
           << ",\"evidence\":" << (evidence.empty() ? "null" : evidence) << "}\n";
    output.flush();
    return ok;
  };
  WNDCLASSW cls{};
  cls.lpfnWndProc = DefWindowProcW;
  cls.hInstance = GetModuleHandleW(nullptr);
  cls.lpszClassName = L"NativePresentEdgeTest";
  RegisterClassW(&cls);
  for (float scale : {1.0f, 1.5f, 2.0f, 3.0f, 4.0f}) {
    auto mode = TestRenderMode();
    mode.scale = {scale, scale};
    const uint32_t width = mode.scale.Width(64), height = mode.scale.Height(36);
    HWND window = CreateWindowW(cls.lpszClassName, L"Presentation edge test", WS_POPUP,
                               0, 0, int(width), int(height), nullptr, nullptr, cls.hInstance, nullptr);
    if (!check("window", window != nullptr, "{}")) return 4;
    Engine engine;
    std::string evidence;
    if (!check("initialize", engine.Initialize(evidence, mode), evidence)) return 5;
    PresentMode present;
    present.vsync = false;
    if (!check("attach", engine.AttachWindow(platform::WindowHandles(window), evidence, present), evidence)) return 6;
    SurfaceDesc surface{0x1234, 64, 36};
    check("surface", engine.CreateSurface(surface, evidence), evidence);
    ClearRequest clear;
    clear.color_keys[0] = surface.key;
    clear.clear_color = true;
    clear.color = {1, 0, 0, 1};
    uint64_t fence = 0;
    check("red-fringe", engine.ClearSurfaces(clear, fence, evidence), evidence);
    clear.color = {0, 1, 0, 1};
    clear.left = clear.top = 1;
    clear.right = 64; clear.bottom = 36;
    check("green-interior", engine.ClearSurfaces(clear, fence, evidence), evidence);
    clear.color = {0, 0, 1, 1}; clear.left = 32; clear.top = 18;
    check("blue-quadrant", engine.ClearSurfaces(clear, fence, evidence), evidence);
    std::vector<uint8_t> before, after;
    uint32_t w = 0, h = 0;
    check("source-before", engine.ReadSurface(surface.key, fence, before, w, h, evidence), evidence);
    for (bool resolved : {false, true}) {
      uint32_t key = surface.key;
      if (resolved) {
        ResolveRequest resolve;
        resolve.source_key = surface.key; resolve.dest_key = key = 0x5678;
        resolve.dest_width = 64; resolve.dest_height = 36; resolve.dest_format = 6;
        check("resolve", engine.ResolveSurface(resolve, fence, evidence), evidence);
      }
      uint32_t slot = 0;
      check("stage", engine.StageFront(key, slot, evidence), evidence);
      std::vector<uint8_t> pixels;
      bool correct = engine.Present(slot, true, pixels, w, h, evidence);
      check("present", correct, evidence);
      correct &= w == width && h == height && pixels.size() == size_t(width) * height * 4;
      if (correct) for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
        const bool red = scale == 1 && (x == 0 || y == 0);
        const bool blue = x >= width / 2 && y >= height / 2;
        const auto* p = pixels.data() + (size_t(y) * width + x) * 4;
        correct &= p[0] == (blue ? 255 : 0) && p[1] == (!red && !blue ? 255 : 0) &&
                   p[2] == (red ? 255 : 0) && p[3] == 255;
      }
      check(resolved ? "texture-output-pixels" : "surface-output-pixels", correct,
            "{\"scale\":" + std::to_string(scale) + "}");
    }
    check("source-after", engine.ReadSurface(surface.key, fence, after, w, h, evidence), evidence);
    check("source-unchanged", before == after, "{}");
    check("release", engine.ReleaseRemaining(evidence), evidence);
    check("detach", engine.DetachWindow(evidence), evidence);
    check("shutdown", engine.Shutdown(evidence), evidence);
    DestroyWindow(window);
  }
  output << "{\"kind\":\"verdict\",\"passed\":" << (passed ? "true" : "false") << "}\n";
  return passed ? 0 : 1;
}
