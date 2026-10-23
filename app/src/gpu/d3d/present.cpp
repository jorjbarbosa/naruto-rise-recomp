// Presentation layer of the D3D boundary.
// The own Plume swapchain lives in the engine, on the SDK window that
// ReXApp::SetupPresentation opens in the native mode (presentation-absent: no
// SDK presenter). The Swap copies the front buffer into a stage slot and posts
// its Present right after the frame lot; the gamma ramp of the title
// (+14988) is applied in the final pass.
#include "gpu/d3d/present.h"
#include "gpu/diagnostics/diagnostics.h"
#include "gpu/d3d/hooks.h"
#include "gpu/backend/dispatcher.h"
#include "gpu/common/guest_memory.h"
#include "gpu/common/sha256.h"
#include "gpu/settings.h"
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
rex::ui::Window* DisplayWindow() {
  auto* runtime = rex::Runtime::instance();
  return runtime ? runtime->display_window() : nullptr;
}
platform::NativeWindow DisplayNativeWindow() {
  auto* window = DisplayWindow();
  return platform::WindowHandles(window ? window->GetNativeWindowHandle() : nullptr);
}
bool InUiThread(std::function<void()> function) {
  auto* runtime = rex::Runtime::instance();
  auto* app = runtime ? runtime->app_context() : nullptr;
  return app && app->CallInUIThreadSynchronous(std::move(function));
}
std::string PresentFile(const std::string& name, const std::vector<uint8_t>& bytes) {
  const char* dir = std::getenv("REX_NATIVE_FRAME_DIR");
  if (!dir || !*dir || bytes.empty()) return "\"file\":null,\"sha256\":null";
  std::FILE* out = std::fopen(fmt::format("{}/{}", dir, name).c_str(), "wb");
  const bool written = out && std::fwrite(bytes.data(), 1, bytes.size(), out) == bytes.size();
  if (out) std::fclose(out);
  return fmt::format("\"file\":\"{}\",\"sha256\":\"{}\"", name,
                     written ? Sha256Hex(bytes.data(), bytes.size()) : std::string("write-failed"));
}

// --- Live presentation ---
// REX_NATIVE_PRESENT_CAPTURES=N reads back the back buffer of the first N
// presents (evidence, waits for the GPU); unset is 0. The presents of the Swaps
// that read their front buffer (swap-NNNN.rgba) are always read back too, as
// present-swap-NNNN-WxH.bgra.
uint64_t PresentCaptures() {
  static const uint64_t value = [] {
    if (!Diagnostics()) return uint64_t(0);
    const char* text = std::getenv("REX_NATIVE_PRESENT_CAPTURES");
    return text && *text ? uint64_t(std::strtoull(text, nullptr, 10)) : uint64_t(0);
  }();
  return value;
}
std::atomic<uint64_t> presents_posted{0}, presents_done{0}, gamma_posts{0};
// REX_NATIVE_CLOSE_AFTER_SWAPS=N: at its own Swap N the render thread
// posts the close of the SDK window's close box (platform::PostWindowClose), so a boot
// exercises the window close without a person. Unset or 0: off.
uint64_t CloseAfterSwaps() {
  static const uint64_t value = [] {
    if (!Diagnostics()) return uint64_t(0);
    const char* text = std::getenv("REX_NATIVE_CLOSE_AFTER_SWAPS");
    return text && *text ? uint64_t(std::strtoull(text, nullptr, 10)) : uint64_t(0);
  }();
  return value;
}
void MaybePostClose(uint64_t swap) {
  if (!CloseAfterSwaps() || swap != CloseAfterSwaps()) return;
  const bool posted = platform::PostWindowClose(DisplayNativeWindow());
  host::Record("native-close-posted", fmt::format("\"swap\":{},\"posted\":{},\"message\":\"close-box\"",
                                                              swap, posted));
}
std::atomic<bool> window_attached{false};
// First own Swap: the swapchain on the SDK window, before the first frame end.
void AttachPresentation(PPCContext& ctx) {
  const platform::NativeWindow window = DisplayNativeWindow();
  if (window.kind == platform::NativeWindow::Kind::kNone) StopInOrder(ctx, "present-window-missing", 88);
  std::string evidence;
  auto* dispatcher = host::ActiveDispatcher();
  // VSync, tearing, buffers and frame latency of the configuration.
  const auto& settings = CurrentSettings();
  PresentMode mode{settings.vsync, settings.allow_tearing, settings.triple_buffering ? 3u : 2u,
                   settings.max_frame_latency};
  // Brightness, the output effect and FXAA.
  mode.brightness = settings.brightness;
  mode.effect = settings.present_effect;
  mode.post_effect = settings.swap_post_effect;
  mode.cas_sharpness = settings.cas_additional_sharpness;
  mode.fsr_sharpness_reduction = settings.fsr_sharpness_reduction;
  mode.fsr_passes = settings.fsr_max_passes;
  const bool attached = dispatcher && dispatcher->Run([window, mode](Engine& engine, std::string& out) {
    return engine.AttachWindow(window, out, mode); }, evidence);
  host::Record("native-present-attach", fmt::format("\"attached\":{},\"engine\":{},{}", attached,
                           evidence.empty() ? "null" : evidence, platform::WindowFacts(window)));
  if (!attached) StopInOrder(ctx, "present-attach-failed", 88);
  window_attached.store(true);
}
// The ramp of the title when SetGammaRamp marked it pending (bit 1 of +16692).
void PostGammaRamp(PPCContext& ctx, uint8_t* base, uint32_t dev) {
  std::array<uint16_t, 768> words{};
  for (uint32_t i = 0; i < 768; ++i) words[i] = Get16(base, dev + 14988 + 2 * i);
  auto* dispatcher = host::ActiveDispatcher();
  const uint64_t n = gamma_posts.fetch_add(1) + 1;
  if (!dispatcher || !dispatcher->Post([words, n](Engine& engine, std::string& out) {
        const bool ok = engine.SetGammaRamp(words, out);
        if (Diagnostics() && n <= 4) host::Record("native-present-gamma", fmt::format("\"post\":{},\"engine\":{}", n, out));
        return ok;
      })) {
    RecordPostedFailure("Swap-gamma-ramp");
    StopInOrder(ctx, "Swap-gamma-ramp-post", 88);
  }
}
// The copy of the front buffer for this Swap's flip, in the frame lot. The
// slot is written on the engine owner and read there by the Present posted at
// the flip (FIFO, same thread).
std::shared_ptr<host::FramePresent> PostStageFront(PPCContext& ctx, uint32_t front, uint64_t swap,
                                                               bool capture) {
  auto present = std::make_shared<host::FramePresent>();
  present->front = front;
  present->swap = swap;
  present->capture = capture;
  auto* dispatcher = host::ActiveDispatcher();
  if (!dispatcher || !dispatcher->Post([present](Engine& engine, std::string& out) {
        return engine.StageFront(present->front, present->slot, out); })) {
    RecordPostedFailure("Swap-stage-front");
    StopInOrder(ctx, "Swap-stage-front-post", 88);
  }
  return present;
}
}  // namespace gpu

namespace gpu {
// Posts the Present of a frame. The own Swap calls it right after its frame
// lot. False only when the Present could not be posted.
bool PresentFrame(const std::shared_ptr<host::FramePresent>& present) {
  if (!present || !window_attached.load()) return true;
  auto* dispatcher = host::ActiveDispatcher();
  const uint64_t n = presents_posted.fetch_add(1) + 1;
  return dispatcher && dispatcher->Post([present, n](Engine& engine, std::string& out) {
    std::vector<uint8_t> bgra;
    uint32_t w = 0, h = 0;
    const bool capture = n <= PresentCaptures() || present->capture;
    const bool ok = engine.Present(present->slot, capture, bgra, w, h, out);
    const uint64_t done = presents_done.fetch_add(1) + 1;
    if (!ok || capture || (Diagnostics() && (done <= 8 || done % 300 == 0)))
      host::Record("native-present-frame", fmt::format(
          "\"present\":{},\"swap\":{},\"front\":{},\"engine\":{},{}", done, present->swap, present->front, out,
          capture ? PresentFile(fmt::format("present-swap-{:04}-{}x{}.bgra", present->swap, w, h), bgra) :
                    std::string("\"file\":null,\"sha256\":null")));
    return ok;
  });
}
std::string PresentSummary() {
  return fmt::format("{{\"attached\":{},\"presents_posted\":{},\"presents_done\":{},\"gamma_posts\":{}}}",
                     window_attached.load(), presents_posted.load(), presents_done.load(), gamma_posts.load());
}
}  // namespace gpu
