#include "gpu/backend/engine.h"
#include "gpu/backend/tests/test_backend.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace gpu;
int main(int argc, char** argv) {
  if (argc != 2 || std::filesystem::exists(argv[1])) return 2;
  std::ofstream out(argv[1]); bool passed = true;
  auto check = [&](const char* kind, bool ok, const std::string& evidence) {
    passed &= ok;
    out << "{\"kind\":\"" << kind << "\",\"ok\":" << (ok ? "true" : "false")
        << ",\"evidence\":" << (evidence.empty() ? "null" : evidence) << "}\n"; out.flush();
  };
  WNDCLASSW cls{}; cls.lpfnWndProc = DefWindowProcW; cls.hInstance = GetModuleHandleW(nullptr);
  cls.lpszClassName = L"NativeVulkanPresentTest"; RegisterClassW(&cls);
  // A borderless real window, no guest or SDK GPU. Tests create it explicitly.
  HWND window = CreateWindowW(cls.lpszClassName, L"Native Vulkan presentation test", WS_POPUP | WS_VISIBLE,
      40, 40, 128, 96, nullptr, nullptr, cls.hInstance, nullptr);
  if (!window) return 3;
  auto pump = [&] { MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } };
  for (unsigned mode = 0; mode < 5; ++mode) {
    Engine engine; std::string evidence;
    auto render = TestRenderMode();
    if (!engine.Initialize(evidence, render)) { check("initialize", false, evidence); break; }
    check("initialize", true, evidence);
    if (mode == 4) {
      std::array<uint16_t, 768> gamma{};
      for (unsigned i = 0; i < 256; ++i)
        gamma[i] = gamma[256 + i] = gamma[512 + i] = uint16_t((1023 - (i * 1023 + 127) / 255) << 6);
      check("gamma-before-attach", engine.SetGammaRamp(gamma, evidence), evidence);
    }
    PresentMode present;
    present.effect = mode == 1 ? PresentEffect::kCas : mode == 2 ? PresentEffect::kFsr : PresentEffect::kBilinear;
    present.post_effect = mode == 3 ? SwapPostEffect::kFxaa : mode == 4 ? SwapPostEffect::kFxaaExtreme : SwapPostEffect::kNone;
    if (!engine.AttachWindow(platform::WindowHandles(window), evidence, present)) { check("attach", false, evidence); engine.Shutdown(evidence); break; }
    check("attach", true, evidence);
    SurfaceDesc surface{0x1234, 64, 48};
    check("surface", engine.CreateSurface(surface, evidence), evidence);
    ClearRequest clear; clear.color_keys[0] = surface.key; clear.clear_color = true;
    clear.color = {1, 0, 0, 1};
    uint64_t fence = 0;
    check("clear", engine.ClearSurfaces(clear, fence, evidence), evidence);
    clear.color = {0, 1, 0, 1}; clear.left = 32; clear.top = 24; clear.right = 64; clear.bottom = 48;
    check("clear-quadrant", engine.ClearSurfaces(clear, fence, evidence), evidence);
    for (unsigned frame = 0; frame < 5; ++frame) {
      if (frame == 1) { SetWindowPos(window, nullptr, 0, 0, 160, 128, SWP_NOMOVE | SWP_NOZORDER); pump(); }
      if (frame == 2) { ShowWindow(window, SW_MINIMIZE); pump(); }
      if (frame == 3) { ShowWindow(window, SW_RESTORE); SetWindowPos(window, nullptr, 0, 0, 128, 96, SWP_NOMOVE | SWP_NOZORDER); pump(); }
      if (frame == 4) check("vsync-off", engine.SetVsync(false, evidence), evidence);
      uint32_t slot{};
      if (!engine.StageFront(surface.key, slot, evidence)) { check("stage", false, evidence); break; }
      check("stage", true, evidence);
      std::vector<uint8_t> pixels; uint32_t w{}, h{};
      const bool shown = engine.Present(slot, frame != 2, pixels, w, h, evidence);
      check("present", shown, evidence);
      if (!shown) break;
      if (frame != 2) {
        bool correct = pixels.size() == size_t(w) * h * 4 && w && h;
        for (uint32_t y : {h / 4, h * 3 / 4}) for (uint32_t x : {w / 4, w * 3 / 4}) {
          if (!correct) continue;
          const auto* p = pixels.data() + (size_t(y) * w + x) * 4;
          const bool green = x > w / 2 && y > h / 2;
          const unsigned blue = mode == 4 ? 255 : 0;
          const unsigned red = (green != (mode == 4)) ? 0 : 255;
          correct &= p[0] == blue && p[1] == 255 - red && p[2] == red && p[3] == 255;
        }
        if (correct && frame == 1) correct &= pixels[0] == 0 && pixels[1] == 0 && pixels[2] == 0 && pixels[3] == 255;
        check("capture-orientation-and-color", correct, "{}");
      }
    }
    check("release", engine.ReleaseRemaining(evidence), evidence);
    check("detach", engine.DetachWindow(evidence), evidence);
    check("shutdown", engine.Shutdown(evidence), evidence);
  }
  DestroyWindow(window);
  out << "{\"kind\":\"verdict\",\"passed\":" << (passed ? "true" : "false") << "}\n";
  return passed ? 0 : 1;
}
