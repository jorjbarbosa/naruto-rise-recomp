// Proof of the own swapchain without the title; the entry
// points (PresentProofRequested, PresentProof) are declared in gpu/d3d/d3d.h.
#include "gpu/d3d/d3d.h"
#include "gpu/diagnostics/diagnostics.h"
#include "gpu/d3d/hooks.h"
#include "gpu/d3d/present.h"
#include "gpu/backend/dispatcher.h"
#include "gpu/host/services.h"
#include "gpu/settings.h"
#include "gpu/common/platform.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>
#include <rex/system/xmemory.h>
#include <functional>
#include <thread>
#include <rex/runtime.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

using namespace gpu;

namespace gpu {
namespace {
// The window helpers of the proof (Windows only, like the proof).
HWND DisplayHwnd() { return static_cast<HWND>(DisplayNativeWindow().window); }
std::pair<uint32_t, uint32_t> ClientSize(HWND hwnd) {
  RECT r{};
  if (!hwnd || !GetClientRect(hwnd, &r)) return {0, 0};
  return {uint32_t(r.right - r.left), uint32_t(r.bottom - r.top)};
}
// Waits (bounded) until the client area has this size.
bool WaitClient(HWND hwnd, uint32_t w, uint32_t h, int ms) {
  for (int i = 0; i < ms / 10; ++i) {
    if (ClientSize(hwnd) == std::pair<uint32_t, uint32_t>{w, h}) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return ClientSize(hwnd) == std::pair<uint32_t, uint32_t>{w, h};
}
// Resizes the window on the UI thread so its client area has w x h pixels.
bool SetClientSize(HWND hwnd, uint32_t w, uint32_t h) {
  bool set = false;
  const bool called = InUiThread([&] {
    RECT window{}, client{};
    if (!GetWindowRect(hwnd, &window) || !GetClientRect(hwnd, &client)) return;
    const LONG fw = (window.right - window.left) - (client.right - client.left);
    const LONG fh = (window.bottom - window.top) - (client.bottom - client.top);
    set = SetWindowPos(hwnd, nullptr, 0, 0, LONG(w) + fw, LONG(h) + fh,
                       SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != 0;
  });
  return called && set && WaitClient(hwnd, w, h, 2000);
}
std::string WindowFacts(HWND hwnd) { return platform::WindowFacts(platform::WindowHandles(hwnd)); }
}  // namespace
uint64_t ProofPercentile(std::vector<uint64_t> values, size_t percent) {
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  const size_t rank = (values.size() * percent + 99) / 100;  // Nearest rank.
  return values[std::max<size_t>(rank, 1) - 1];
}

// --- Swapchain proof without the title ---
// REX_NATIVE_PRESENT_PROOF=1: right after the engine is ready (VdInitializeEngines),
// on the SDK window and before any title frame. A 1280 x 720 pattern (four
// bands of 180 rows: gray, red, green, blue; level x / 5) is presented with the
// gamma ramp of REX_NATIVE_PRESENT_PROOF_RAMP (768 decimal words) in several
// client sizes, with vsync on and off, in fullscreen and back; the back buffer
// of each case is read back into REX_NATIVE_FRAME_DIR for an external checker.
// A front buffer with undefined content (a deferred depth resolve) must be
// refused. Then the
// swapchain is released and the process stops in order (86).
bool PresentProofRequested() {
  if (!Diagnostics()) return false;
  const char* text = std::getenv("REX_NATIVE_PRESENT_PROOF");
  return text && std::string_view(text) == "1";
}
[[noreturn]] void PresentProof(PPCContext& ctx) {
  constexpr uint32_t kPattern = 0xFFFF0001u, kDepth = 0xFFFF0002u, kUndefined = 0xFFFF0003u;
  auto* dispatcher = host::ActiveDispatcher();
  HWND hwnd = DisplayHwnd();
  auto* window = DisplayWindow();
  bool all = dispatcher && hwnd && window;
  std::string cases;
  auto record = [&](std::string_view name, bool ok, const std::string& fields) {
    all = all && ok;
    host::Record("native-present-proof-step", fmt::format("\"step\":\"{}\",\"ok\":{},{}", name, ok, fields));
    cases += fmt::format("{}{{\"step\":\"{}\",\"ok\":{},{}}}", cases.empty() ? "" : ",", name, ok, fields);
  };
  auto run = [&](std::function<bool(Engine&, std::string&)> operation, std::string& evidence) {
    evidence.clear();
    const bool ok = dispatcher && dispatcher->Run(std::move(operation), evidence);
    if (evidence.empty()) evidence = "null";
    return ok;
  };
  if (!all) {
    record("window", false, fmt::format("\"dispatcher\":{},\"hwnd\":{}", dispatcher != nullptr,
                                        reinterpret_cast<uintptr_t>(hwnd)));
    StopInOrder(ctx, "native-present-proof-no-window", 88);
  }
  record("window", true, WindowFacts(hwnd));
  // A ramp of the title, recorded at a Swap.
  std::array<uint16_t, 768> words{};
  size_t count = 0;
  if (const char* path = std::getenv("REX_NATIVE_PRESENT_PROOF_RAMP"); path && *path)
    if (std::FILE* in = std::fopen(path, "rb")) {
      unsigned value = 0;
      while (count < words.size() && std::fscanf(in, "%u", &value) == 1) words[count++] = uint16_t(value);
      std::fclose(in);
    }
  std::string evidence;
  bool ok = count == 768 && run([&](Engine& engine, std::string& out) {
    return engine.SetGammaRamp(words, out); }, evidence);
  record("gamma", ok, fmt::format("\"words\":{},\"engine\":{}", count, evidence));
  // Pattern, by clears (exact UNORM values of level / 255).
  ok = run([&](Engine& engine, std::string& out) {
    if (!engine.CreateSurface({kPattern, 1280, 720, 0, 1, SurfaceFormat::Color8888}, out)) return false;
    for (uint32_t band = 0; band < 4; ++band)
      for (uint32_t level = 0; level < 256; ++level) {
        ClearRequest clear;
        clear.color_keys[0] = kPattern;
        clear.clear_color = true;
        const float v = float(level) / 255.0f;
        clear.color = {band == 0 || band == 1 ? v : 0.0f, band == 0 || band == 2 ? v : 0.0f,
                       band == 0 || band == 3 ? v : 0.0f, 1.0f};
        clear.left = int32_t(5 * level);
        clear.right = clear.left + 5;
        clear.top = int32_t(180 * band);
        clear.bottom = clear.top + 180;
        uint64_t fence = 0;
        if (!engine.ClearSurfaces(clear, fence, out)) return false;
      }
    return engine.WaitIdle(out);
  }, evidence);
  record("pattern", ok, fmt::format("\"clears\":1024,\"engine\":{}", evidence));
  ok = run([&](Engine& engine, std::string& out) { return engine.AttachWindow(platform::WindowHandles(hwnd), out); }, evidence);
  record("attach", ok, fmt::format("\"engine\":{}", evidence));
  if (!all) StopInOrder(ctx, "native-present-proof-setup-failed", 88);
  // One case: stage, present once (follows the window size), then present and read back.
  auto present_case = [&](const std::string& name) {
    std::vector<uint8_t> bgra;
    uint32_t w = 0, h = 0;
    std::string first, second;
    const bool presented = run([&](Engine& engine, std::string& out) {
      uint32_t slot = 0;
      std::vector<uint8_t> ignored;
      uint32_t iw = 0, ih = 0;
      return engine.StageFront(kPattern, slot, out) && engine.Present(slot, false, ignored, iw, ih, out); }, first) &&
        run([&](Engine& engine, std::string& out) {
          uint32_t slot = 0;
          return engine.StageFront(kPattern, slot, out) && engine.Present(slot, true, bgra, w, h, out); }, second);
    record(name, presented && w && h, fmt::format("\"first\":{},\"engine\":{},{},{}", first, second,
           PresentFile(fmt::format("present-proof-{}-{}x{}.bgra", name, w, h), bgra), WindowFacts(hwnd)));
  };
  auto resize_case = [&](const std::string& name, uint32_t w, uint32_t h) {
    const bool sized = SetClientSize(hwnd, w, h);
    record(name + "-resize", sized, fmt::format("\"requested\":[{},{}],{}", w, h, WindowFacts(hwnd)));
    present_case(name);
  };
  resize_case("1280x720", 1280, 720);
  // Vsync: intervals between consecutive presents of the same pattern.
  auto timing = [&](bool vsync, uint32_t frames) {
    std::string set;
    bool good = run([&](Engine& engine, std::string& out) { return engine.SetVsync(vsync, out); }, set);
    std::vector<int64_t> qpc;
    LARGE_INTEGER frequency{}, t{};
    QueryPerformanceFrequency(&frequency);
    for (uint32_t i = 0; good && i < frames; ++i) {
      good = run([&](Engine& engine, std::string& out) {
        uint32_t slot = 0, iw = 0, ih = 0;
        std::vector<uint8_t> ignored;
        return engine.StageFront(kPattern, slot, out) && engine.Present(slot, false, ignored, iw, ih, out); }, evidence);
      QueryPerformanceCounter(&t);
      qpc.push_back(t.QuadPart);
    }
    std::vector<uint64_t> us;
    for (size_t i = 1; i < qpc.size(); ++i) us.push_back(uint64_t((qpc[i] - qpc[i - 1]) * 1000000 / frequency.QuadPart));
    record(vsync ? "vsync-on" : "vsync-off", good, fmt::format(
        "\"frames\":{},\"interval_us\":{{\"p5\":{},\"median\":{},\"p95\":{},\"max\":{}}},\"intervals_us\":[{}],"
        "\"set\":{},\"last\":{},{}", frames, ProofPercentile(us, 5), ProofPercentile(us, 50), ProofPercentile(us, 95),
        ProofPercentile(us, 100), fmt::join(us, ","), set, evidence, WindowFacts(hwnd)));
  };
  timing(true, 121);
  timing(false, 121);
  const bool vsync = CurrentSettings().vsync;  // Back to the configured VSync.
  ok = run([&](Engine& engine, std::string& out) { return engine.SetVsync(vsync, out); }, evidence);
  record("vsync-restored", ok, fmt::format("\"engine\":{}", evidence));
  resize_case("1600x720", 1600, 720);   // Pillarbox, image at scale 1.
  resize_case("960x720", 960, 720);     // Letterbox, scale 3/4.
  resize_case("640x360", 640, 360);     // Scale 1/2.
  // Fullscreen by the SDK window (SDL), then back.
  const auto monitor_size = [&] {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info);
    return std::pair<uint32_t, uint32_t>{uint32_t(info.rcMonitor.right - info.rcMonitor.left),
                                         uint32_t(info.rcMonitor.bottom - info.rcMonitor.top)};
  }();
  ok = InUiThread([window] { window->SetFullscreen(true); }) &&
       WaitClient(hwnd, monitor_size.first, monitor_size.second, 3000);
  record("fullscreen-enter", ok, fmt::format("\"monitor_size\":[{},{}],{}", monitor_size.first, monitor_size.second,
                                             WindowFacts(hwnd)));
  present_case("fullscreen");
  ok = InUiThread([window] { window->SetFullscreen(false); });
  for (int i = 0; ok && i < 300 && ClientSize(hwnd) == monitor_size; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  record("fullscreen-leave", ok && ClientSize(hwnd) != monitor_size, WindowFacts(hwnd));
  resize_case("restored", 1280, 720);
  // A front buffer whose content is undefined (deferred depth resolve) is refused.
  std::string refused;
  ok = run([&](Engine& engine, std::string& out) {
    std::string created;
    uint64_t fence = 0;
    ResolveRequest resolve;
    resolve.source_key = kDepth;
    resolve.dest_key = kUndefined;
    resolve.dest_width = 1280;
    resolve.dest_height = 720;
    if (!engine.CreateSurface({kDepth, 1280, 720, 0, 1, SurfaceFormat::DepthStencil}, created) ||
        !engine.ResolveSurface(resolve, fence, out)) return false;
    uint32_t slot = 0;
    const bool staged = engine.StageFront(kUndefined, slot, refused);
    std::string destroyed;
    return !staged && engine.DestroySurface(kDepth, destroyed);
  }, evidence);
  record("a11-undefined-refused", ok && refused.find("native-present-content-undefined") != std::string::npos,
         fmt::format("\"resolve\":{},\"stage\":{}", evidence, refused.empty() ? "null" : refused));
  // D750: the own texture of a resolve destination released while its
  // resolve is still in the open batch is retired, and released after the fence.
  constexpr uint32_t kResolved = 0xFFFF0004u;
  std::string release, before, retired, after;
  ok = run([&](Engine& engine, std::string& out) {
    ResolveRequest resolve;
    resolve.source_key = kPattern;
    resolve.dest_key = kResolved;
    resolve.dest_width = 1280;
    resolve.dest_height = 720;
    uint64_t fence = 0;
    if (!engine.ResolveSurface(resolve, fence, out)) return false;
    before = engine.ResourceCounts();
    if (!engine.ReleaseTexture(kResolved, release)) return false;
    retired = engine.ResourceCounts();
    if (!engine.WaitIdle(out)) return false;
    after = engine.ResourceCounts();
    return release.find("\"deferred\":true") != std::string::npos &&
           retired.find("\"retired_textures\":1") != std::string::npos &&
           after.find("\"retired_textures\":0") != std::string::npos;
  }, evidence);
  record("d750-deferred", ok, fmt::format("\"engine\":{},\"before\":{},\"release\":{},\"retired\":{},\"after\":{}",
                                          evidence, before.empty() ? "null" : before, release.empty() ? "null" : release,
                                          retired.empty() ? "null" : retired, after.empty() ? "null" : after));
  // Release: the swapchain after the queue is idle, then the pattern.
  ok = run([&](Engine& engine, std::string& out) {
    std::string destroyed;
    return engine.DetachWindow(out) && engine.DestroySurface(kPattern, destroyed); }, evidence);
  record("detach", ok, fmt::format("\"engine\":{}", evidence));
  std::string counts;
  run([&](Engine& engine, std::string& out) { out = engine.ResourceCounts(); return true; }, counts);
  record("resources-after-detach", true, fmt::format("\"engine\":{}", counts));
  const char* dir = std::getenv("REX_NATIVE_FRAME_DIR");
  std::string summary = fmt::format("{{\"schema\":\"native-4-4c-present-proof-v1\",\"pattern\":\"1280x720, bands of "
                                    "180 rows gray/red/green/blue, level x/5\",\"ramp\":\"{}\",\"all_ok\":{},"
                                    "\"steps\":[{}]}}\n", std::getenv("REX_NATIVE_PRESENT_PROOF_RAMP") ?
                                    std::getenv("REX_NATIVE_PRESENT_PROOF_RAMP") : "", all, cases);
  for (char& c : summary) if (c == '\\') c = '/';
  if (dir && *dir) {
    std::vector<uint8_t> bytes(summary.begin(), summary.end());
    host::Record("native-present-proof", fmt::format("\"all_ok\":{},{}", all,
                             PresentFile("present-proof.json", bytes)));
  }
  StopInOrder(ctx, all ? "native-present-proof-complete" : "native-present-proof-failed", all ? 86 : 88);
}
}  // namespace gpu
