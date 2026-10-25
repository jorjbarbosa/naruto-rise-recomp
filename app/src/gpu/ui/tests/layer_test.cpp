// GPU check of the UI layer (ui/layer.h) and the SDK's immediate drawing over
// it (ui/immediate_drawer.h), through a dispatcher as the Swaps deliver it.
// Over a green image: an indexed batch with an opaque red quad, a batch
// without indices with a quad of a 2 x 1 texture (magenta, cyan), a batch
// whose draw starts past its first vertex, cut by a fractional scissor rect (a
// pixel is in when its center is), and a line, not drawn. Then the texture
// released with its object and an empty frame; and a second engine, after
// ForgetEngine, gets the live texture and the frame without new calls.
#include "gpu/backend/dispatcher.h"
#include "gpu/backend/tests/test_backend.h"
#include "gpu/ui/immediate_drawer.h"
#include "gpu/ui/layer.h"
#include "menu/setup_gate.h"
#include <windows.h>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <rex/ui/presenter.h>

using namespace gpu;

namespace gpu::sampler {
void RegisterThread(const char*) {}  // The sampler is not linked here.
}  // namespace gpu::sampler

namespace {
constexpr uint32_t kWidth = 64, kHeight = 36, kSurface = 0x1234;
using Rgba = std::array<int, 4>;
constexpr Rgba kGreen{0, 255, 0, 255}, kRed{255, 0, 0, 255}, kWhite{255, 255, 255, 255};
constexpr Rgba kMagenta{255, 0, 255, 255}, kCyan{0, 255, 255, 255};

constexpr uint32_t Color(int r, int g, int b, int a) {
  return uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16 | uint32_t(a) << 24;
}
void Quad(std::vector<rex::ui::ImmediateVertex>& v, float x0, float y0, float x1, float y1, uint32_t color) {
  v.push_back({x0, y0, 0, 0, color});
  v.push_back({x1, y0, 1, 0, color});
  v.push_back({x1, y1, 1, 1, color});
  v.push_back({x0, y1, 0, 1, color});
}
bool Inside(uint32_t x, uint32_t y, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
  return x >= x0 && x < x1 && y >= y0 && y < y1;
}
bool Matches(const std::vector<uint8_t>& bgra, const std::function<std::optional<Rgba>(uint32_t, uint32_t)>& expected,
             std::string& where) {
  for (uint32_t y = 0; y < kHeight; ++y)
    for (uint32_t x = 0; x < kWidth; ++x) {
      const auto want = expected(x, y);
      if (!want) continue;
      const uint8_t* p = bgra.data() + (size_t(y) * kWidth + x) * 4;
      const Rgba got{p[2], p[1], p[0], p[3]};
      for (int c = 0; c < 4; ++c)
        if (std::abs(got[c] - (*want)[c]) > 1) {
          where = "{\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) + ",\"got\":[" + std::to_string(got[0]) +
                  "," + std::to_string(got[1]) + "," + std::to_string(got[2]) + "," + std::to_string(got[3]) + "]}";
          return false;
        }
    }
  where = "{}";
  return true;
}
std::optional<Rgba> Shapes(uint32_t x, uint32_t y) {
  if (Inside(x, y, 4, 4, 20, 12)) return kRed;
  if (Inside(x, y, 4, 16, 12, 24)) return kMagenta;
  if (Inside(x, y, 12, 16, 28, 24)) return std::nullopt;  // Between the texels.
  if (Inside(x, y, 28, 16, 36, 24)) return kCyan;
  if (Inside(x, y, 44, 16, 52, 24)) return kWhite;
  return kGreen;
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
  cls.lpszClassName = L"NativeUiLayerTest";
  RegisterClassW(&cls);
  HWND window = CreateWindowW(cls.lpszClassName, L"UI layer test", WS_POPUP, 0, 0, int(kWidth), int(kHeight),
                              nullptr, nullptr, cls.hInstance, nullptr);
  if (!check("window", window != nullptr, "{}")) return 4;
  ShowWindow(window, SW_SHOWNOACTIVATE);

  ui::Layer layer;
  ui::ImmediateDrawer drawer(layer);
  rex::ui::AppUIDrawContext context(kWidth, kHeight);
  int requests = 0;
  layer.SetFrameRequest([&] { ++requests; });
  const auto marker = std::filesystem::path(std::string(argv[1]) + ".launch.marker");
  std::ofstream(marker) << "started\n";
  int confirmed = 0;
  layer.SetFirstPresentCallback([&] {
    ++confirmed;
    std::error_code error;
    std::filesystem::remove(marker, error);
  });
  const auto recovery_reason = [&] {
    narutorise::menu::SetupFacts facts;
    facts.found = facts.configured = true;
    facts.status = narutorise::game_image::Status::kReady;
    facts.start_failed = std::filesystem::exists(marker);
    return narutorise::menu::SetupReasonFor(facts);
  };
  const std::vector<uint8_t> texels{255, 0, 255, 255, 0, 255, 255, 255};  // Magenta, cyan.
  auto texture = drawer.CreateTexture(2, 1, rex::ui::ImmediateTextureFilter::kLinear, false, texels.data());
  check("texture", texture != nullptr, "{}");
  // The shapes, in the render target's coordinates (no coordinate space given).
  drawer.Begin(context, 0, 0);
  {
    std::vector<rex::ui::ImmediateVertex> v;
    Quad(v, 4, 4, 20, 12, Color(255, 0, 0, 255));
    const uint16_t indices[] = {0, 1, 2, 0, 2, 3};
    drawer.BeginDrawBatch({v.data(), int(v.size()), indices, 6});
    rex::ui::ImmediateDraw draw;
    draw.count = 6;
    drawer.Draw(draw);
    drawer.EndDrawBatch();
  }
  {
    std::vector<rex::ui::ImmediateVertex> quad, v;
    Quad(quad, 4, 16, 36, 24, Color(255, 255, 255, 255));
    for (int i : {0, 1, 2, 0, 2, 3}) v.push_back(quad[i]);
    drawer.BeginDrawBatch({v.data(), int(v.size()), nullptr, 0});
    rex::ui::ImmediateDraw draw;
    draw.count = 6;
    draw.texture = texture.get();
    drawer.Draw(draw);
    drawer.EndDrawBatch();
  }
  {
    std::vector<rex::ui::ImmediateVertex> v;
    Quad(v, 0, 0, 8, 8, Color(0, 0, 255, 255));  // Not drawn: the draw starts at vertex 4.
    Quad(v, 0, 0, float(kWidth), float(kHeight), Color(255, 255, 255, 255));
    const uint16_t indices[] = {0, 1, 2, 0, 2, 3};
    drawer.BeginDrawBatch({v.data(), int(v.size()), indices, 6});
    rex::ui::ImmediateDraw draw;
    draw.count = 6;
    draw.base_vertex = 4;
    draw.scissor = true;
    draw.scissor_left = 44.4f; draw.scissor_top = 15.6f; draw.scissor_right = 51.6f; draw.scissor_bottom = 24.4f;
    drawer.Draw(draw);
    rex::ui::ImmediateDraw line;
    line.primitive_type = rex::ui::ImmediatePrimitiveType::kLines;
    line.count = 2;
    drawer.Draw(line);
    drawer.EndDrawBatch();
  }
  drawer.End();

  auto engine_case = [&](const std::string& name, const std::function<void(Dispatcher&)>& frames) {
    Dispatcher dispatcher(true);
    std::string evidence;
    if (!check(name + "-initialize", dispatcher.Initialize(evidence, TestRenderMode()), evidence)) return;
    PresentMode present;
    present.vsync = false;
    check(name + "-attach", dispatcher.Run([&](Engine& e, std::string& out) {
      return e.AttachWindow(window, out, present) && e.CreateSurface(SurfaceDesc{kSurface, kWidth, kHeight}, out);
    }, evidence), evidence);
    ClearRequest clear;
    clear.color_keys[0] = kSurface;
    clear.clear_color = true;
    clear.color = {0, 1, 0, 1};
    check(name + "-green", dispatcher.Run([&](Engine& e, std::string& out) {
      uint64_t fence = 0;
      return e.ClearSurfaces(clear, fence, out);
    }, evidence), evidence);
    frames(dispatcher);
    check(name + "-release", dispatcher.Run([](Engine& e, std::string& out) {
      return e.ReleaseRemaining(out) && e.DetachWindow(out);
    }, evidence), evidence);
    check(name + "-shutdown", dispatcher.Shutdown(evidence), evidence);
  };
  // One Swap: the delivery, then the Present read back.
  auto swap = [&](Dispatcher& dispatcher, const std::string& kind,
                  const std::function<std::optional<Rgba>(uint32_t, uint32_t)>& expected) {
    std::string evidence;
    check(kind + "-deliver", layer.Deliver(dispatcher), "{}");
    std::vector<uint8_t> pixels;
    uint32_t w = 0, h = 0;
    const bool presented = dispatcher.Run([&](Engine& e, std::string& out) {
      uint32_t slot = 0;
      if (!e.StageFront(kSurface, slot, out)) return false;
      bool shown = false;
      const bool ok = e.Present(slot, true, pixels, w, h, out, &shown);
      layer.NotifyPresented(ok, shown);
      return ok && shown;
    }, evidence);
    if (!check(kind + "-present", presented, evidence)) return;
    std::string where;
    check(kind + "-pixels", w == kWidth && h == kHeight && Matches(pixels, expected, where), where);
  };
  engine_case("first", [&](Dispatcher& dispatcher) {
    check("before-first-present-deliver", layer.Deliver(dispatcher), "{}");
    check("delivery-does-not-remove-launch-marker", confirmed == 0 && std::filesystem::exists(marker), "{}");
    bool shown = true;
    std::string evidence;
    const bool failed_present = dispatcher.Run([&](Engine& e, std::string& out) {
      std::vector<uint8_t> ignored;
      uint32_t w = 0, h = 0;
      const bool ok = e.Present(UINT32_MAX, false, ignored, w, h, out, &shown);
      layer.NotifyPresented(ok, shown);
      return ok;
    }, evidence);
    check("failed-first-present-keeps-recovery", !failed_present && !shown && confirmed == 0 &&
          recovery_reason() == narutorise::menu::SetupReason::kStartFailed, evidence);
    SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    shown = true;
    const bool skipped_present = dispatcher.Run([&](Engine& e, std::string& out) {
      std::vector<uint8_t> ignored;
      uint32_t w = 0, h = 0, slot = 0;
      if (!e.StageFront(kSurface, slot, out)) return false;
      const bool ok = e.Present(slot, false, ignored, w, h, out, &shown);
      layer.NotifyPresented(ok, shown);
      return ok;
    }, evidence);
    check("skipped-first-present-keeps-recovery", skipped_present && !shown && confirmed == 0 &&
          recovery_reason() == narutorise::menu::SetupReason::kStartFailed, evidence);
    SetWindowPos(window, nullptr, 0, 0, int(kWidth), int(kHeight), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    swap(dispatcher, "shapes", Shapes);
    check("successful-present-clears-launch-marker", confirmed == 1 &&
          recovery_reason() == narutorise::menu::SetupReason::kNone, "{}");
    swap(dispatcher, "unchanged", Shapes);  // Nothing new: the engine keeps the frame.
    check("first-present-callback-is-one-shot", confirmed == 1, "{}");
    texture.reset();
    drawer.Begin(context, float(kWidth), float(kHeight));
    drawer.End();
    swap(dispatcher, "empty", [](uint32_t, uint32_t) { return kGreen; });
    std::string counts;
    dispatcher.Run([](Engine& e, std::string& out) { out = e.ResourceCounts(); return true; }, counts);
    check("released", counts.find("\"ui_textures\":0,") != std::string::npos, counts);
  });
  check("requests", requests == 4, "{\"requests\":" + std::to_string(requests) + "}");
  layer.SetFirstPresentCallback([&] { ++confirmed; });
  layer.SetFirstPresentCallback({});
  layer.NotifyPresented(true, true);
  check("cleared-present-callback-is-not-invoked", confirmed == 1, "{}");

  // A live texture and a frame drawn with it, then a second engine.
  texture = drawer.CreateTexture(2, 1, rex::ui::ImmediateTextureFilter::kLinear, false, texels.data());
  drawer.Begin(context, float(kWidth) / 2, float(kHeight) / 2);
  {
    std::vector<rex::ui::ImmediateVertex> v;
    Quad(v, 2, 8, 18, 12, Color(255, 255, 255, 255));  // Pixels (4, 16) to (36, 24).
    const uint16_t indices[] = {0, 1, 2, 0, 2, 3};
    drawer.BeginDrawBatch({v.data(), int(v.size()), indices, 6});
    rex::ui::ImmediateDraw draw;
    draw.count = 6;
    draw.texture = texture.get();
    drawer.Draw(draw);
    drawer.EndDrawBatch();
  }
  drawer.End();
  auto texture_only = [](uint32_t x, uint32_t y) -> std::optional<Rgba> {
    if (Inside(x, y, 4, 16, 12, 24)) return kMagenta;
    if (Inside(x, y, 12, 16, 28, 24)) return std::nullopt;
    if (Inside(x, y, 28, 16, 36, 24)) return kCyan;
    return kGreen;
  };
  engine_case("before-forget", [&](Dispatcher& dispatcher) { swap(dispatcher, "half-space", texture_only); });
  layer.ForgetEngine();
  engine_case("after-forget", [&](Dispatcher& dispatcher) { swap(dispatcher, "redelivered", texture_only); });
  texture.reset();
  DestroyWindow(window);
  output << "{\"kind\":\"verdict\",\"passed\":" << (passed ? "true" : "false") << "}\n";
  return passed ? 0 : 1;
}
