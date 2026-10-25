// GPU check of the UI layer of the Present (UiFrame). Over a green image, a
// frame draws an opaque red rect, a half-transparent blue rect (straight
// alpha), a quad of a 2 x 1 UI texture (magenta, cyan) and a white quad cut by
// its scissor rect; the back buffer read back must hold them and green
// elsewhere. Then a coordinate space of half the back buffer, a texture
// replaced at another size, a released texture (its draws skipped), an empty
// frame, and the refusals of the engine. Without and with an output effect.
#include "gpu/backend/engine.h"
#include "gpu/backend/tests/test_backend.h"
#include <windows.h>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using namespace gpu;

namespace {
constexpr uint32_t kWidth = 64, kHeight = 36, kSurface = 0x1234, kTexture = 7;
using Rgba = std::array<int, 4>;
constexpr Rgba kGreen{0, 255, 0, 255}, kRed{255, 0, 0, 255}, kWhite{255, 255, 255, 255};
constexpr Rgba kMagenta{255, 0, 255, 255}, kCyan{0, 255, 255, 255}, kYellow{255, 255, 0, 255};
constexpr Rgba kHalfBlue{0, 127, 128, 255};  // 128/255 blue over green.

// A vertex color, R8G8B8A8 with red in the low byte.
constexpr uint32_t Color(int r, int g, int b, int a) {
  return uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16 | uint32_t(a) << 24;
}

UiDraw& Quad(UiFrame& f, float x0, float y0, float x1, float y1, uint32_t color, uint32_t texture = 0) {
  UiDraw d;
  d.texture = texture;
  d.vertex_offset = uint32_t(f.vertices.size());
  d.index_offset = uint32_t(f.indices.size());
  d.index_count = 6;
  f.vertices.push_back({x0, y0, 0, 0, color});
  f.vertices.push_back({x1, y0, 1, 0, color});
  f.vertices.push_back({x1, y1, 1, 1, color});
  f.vertices.push_back({x0, y1, 0, 1, color});
  for (uint16_t i : {0, 1, 2, 0, 2, 3}) f.indices.push_back(i);
  return f.draws.emplace_back(d);
}

bool Inside(uint32_t x, uint32_t y, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
  return x >= x0 && x < x1 && y >= y0 && y < y1;
}

// Every pixel against its expected color (none: not checked), within tolerance;
// the first mismatch goes into where.
bool Matches(const std::vector<uint8_t>& bgra, const std::function<std::optional<Rgba>(uint32_t, uint32_t)>& expected,
             int tolerance, std::string& where) {
  for (uint32_t y = 0; y < kHeight; ++y)
    for (uint32_t x = 0; x < kWidth; ++x) {
      const auto want = expected(x, y);
      if (!want) continue;
      const uint8_t* p = bgra.data() + (size_t(y) * kWidth + x) * 4;
      const Rgba got{p[2], p[1], p[0], p[3]};
      for (int c = 0; c < 4; ++c)
        if (std::abs(got[c] - (*want)[c]) > tolerance) {
          where = "{\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) + ",\"got\":[" + std::to_string(got[0]) +
                  "," + std::to_string(got[1]) + "," + std::to_string(got[2]) + "," + std::to_string(got[3]) +
                  "],\"expected\":[" + std::to_string((*want)[0]) + "," + std::to_string((*want)[1]) + "," +
                  std::to_string((*want)[2]) + "," + std::to_string((*want)[3]) + "]}";
          return false;
        }
    }
  where = "{}";
  return true;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 || std::filesystem::exists(argv[1])) return 2;
  std::ofstream output(argv[1]);
  if (!output) return 3;
  bool passed = true;
  auto check = [&](const std::string& kind, bool ok, const std::string& evidence) {
    passed &= ok;
    output << "{\"kind\":\"" << kind << "\",\"ok\":" << (ok ? "true" : "false")
           << ",\"evidence\":" << (evidence.empty() ? "null" : evidence) << "}\n";
    output.flush();
    return ok;
  };
  WNDCLASSW cls{};
  cls.lpfnWndProc = DefWindowProcW;
  cls.hInstance = GetModuleHandleW(nullptr);
  cls.lpszClassName = L"NativeUiTest";
  RegisterClassW(&cls);
  for (const bool fxaa : {false, true}) {
    const std::string mode_name = fxaa ? "fxaa-" : "";
    HWND window = CreateWindowW(cls.lpszClassName, L"UI layer test", WS_POPUP, 0, 0, int(kWidth), int(kHeight),
                                nullptr, nullptr, cls.hInstance, nullptr);
    if (!check("window", window != nullptr, "{}")) return 4;
    Engine engine;
    std::string evidence;
    if (!check("initialize", engine.Initialize(evidence, TestRenderMode()), evidence)) return 5;
    PresentMode present;
    present.vsync = false;
    if (fxaa) present.post_effect = SwapPostEffect::kFxaa;
    if (!check("attach", engine.AttachWindow(window, evidence, present), evidence)) return 6;
    check("surface", engine.CreateSurface(SurfaceDesc{kSurface, kWidth, kHeight}, evidence), evidence);
    ClearRequest clear;
    clear.color_keys[0] = kSurface;
    clear.clear_color = true;
    clear.color = {0, 1, 0, 1};
    uint64_t fence = 0;
    check("green-image", engine.ClearSurfaces(clear, fence, evidence), evidence);
    // Stages the green image, presents it with the UI layer and checks the back buffer.
    auto frame = [&](const std::string& kind, int tolerance,
                     const std::function<std::optional<Rgba>(uint32_t, uint32_t)>& expected) {
      uint32_t slot = 0, w = 0, h = 0;
      std::vector<uint8_t> pixels;
      bool ok = engine.StageFront(kSurface, slot, evidence) && engine.Present(slot, true, pixels, w, h, evidence);
      if (!check(mode_name + kind + "-present", ok, evidence)) return;
      std::string where;
      ok = w == kWidth && h == kHeight && pixels.size() == size_t(kWidth) * kHeight * 4 &&
           Matches(pixels, expected, tolerance, where);
      check(mode_name + kind + "-pixels", ok, where);
    };
    frame("no-frame", 1, [](uint32_t, uint32_t) { return kGreen; });

    const std::vector<uint8_t> texture{255, 0, 255, 255, 0, 255, 255, 255};  // Magenta, cyan.
    check("upload-texture", engine.UploadUiTexture(kTexture, 2, 1, texture, evidence), evidence);
    UiFrame shapes;
    shapes.width = float(kWidth);
    shapes.height = float(kHeight);
    Quad(shapes, 4, 4, 20, 12, Color(255, 0, 0, 255));
    Quad(shapes, 24, 4, 40, 12, Color(0, 0, 255, 128));
    Quad(shapes, 4, 16, 36, 24, Color(255, 255, 255, 255), kTexture);
    UiDraw& cut = Quad(shapes, 0, 0, float(kWidth), float(kHeight), Color(255, 255, 255, 255));
    cut.scissor = true;
    cut.scissor_left = 44; cut.scissor_top = 16; cut.scissor_right = 52; cut.scissor_bottom = 24;
    check("set-shapes", engine.SetUiFrame(shapes, evidence), evidence);
    // Texel centers: u <= 0.25 is the left texel alone, u >= 0.75 the right one.
    frame("shapes", 2, [](uint32_t x, uint32_t y) -> std::optional<Rgba> {
      if (Inside(x, y, 4, 4, 20, 12)) return kRed;
      if (Inside(x, y, 24, 4, 40, 12)) return kHalfBlue;
      if (Inside(x, y, 4, 16, 12, 24)) return kMagenta;
      if (Inside(x, y, 12, 16, 28, 24)) return std::nullopt;
      if (Inside(x, y, 28, 16, 36, 24)) return kCyan;
      if (Inside(x, y, 44, 16, 52, 24)) return kWhite;
      return kGreen;
    });

    UiFrame half;
    half.width = kWidth / 2.0f;
    half.height = kHeight / 2.0f;
    Quad(half, 0, 0, kWidth / 4.0f, kHeight / 4.0f, Color(255, 0, 0, 255));
    check("set-half-space", engine.SetUiFrame(half, evidence), evidence);
    frame("half-space", 1, [](uint32_t x, uint32_t y) {
      return Inside(x, y, 0, 0, kWidth / 2, kHeight / 2) ? kRed : kGreen;
    });

    check("replace-texture", engine.UploadUiTexture(kTexture, 1, 1, {255, 255, 0, 255}, evidence), evidence);
    UiFrame replaced;
    replaced.width = float(kWidth);
    replaced.height = float(kHeight);
    Quad(replaced, 4, 4, 20, 12, Color(255, 255, 255, 255), kTexture);
    check("set-replaced", engine.SetUiFrame(replaced, evidence), evidence);
    frame("replaced-texture", 1, [](uint32_t x, uint32_t y) {
      return Inside(x, y, 4, 4, 20, 12) ? kYellow : kGreen;
    });

    check("release-texture", engine.ReleaseUiTexture(kTexture, evidence), evidence);
    frame("released-texture", 1, [](uint32_t, uint32_t) { return kGreen; });
    const std::string counts = engine.ResourceCounts();
    check("released-counts", counts.find("\"ui_textures\":0,") != std::string::npos &&
                                 counts.find("\"ui_draws_skipped\":1") != std::string::npos, counts);

    check("set-empty", engine.SetUiFrame(UiFrame{}, evidence), evidence);
    frame("empty-frame", 1, [](uint32_t, uint32_t) { return kGreen; });

    UiFrame outside = replaced;
    outside.indices[2] = 4;  // Four vertices.
    check("refuse-vertex-range", !engine.SetUiFrame(outside, evidence) &&
                                     evidence.find("native-ui-frame-vertices") != std::string::npos, evidence);
    check("refuse-key-0", !engine.UploadUiTexture(0, 1, 1, {255, 255, 255, 255}, evidence), evidence);
    check("refuse-size", !engine.UploadUiTexture(9, 2, 2, std::vector<uint8_t>(15), evidence), evidence);

    check("release", engine.ReleaseRemaining(evidence), evidence);
    check("detach", engine.DetachWindow(evidence), evidence);
    check("shutdown", engine.Shutdown(evidence), evidence);
    DestroyWindow(window);
  }
  output << "{\"kind\":\"verdict\",\"passed\":" << (passed ? "true" : "false") << "}\n";
  return passed ? 0 : 1;
}
