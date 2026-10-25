// A screen of the UI before the game runs (screen.h).
#include "gpu/ui/screen.h"
#include "gpu/backend/dispatcher.h"
#include "gpu/ui/layer.h"
#include <chrono>
#include <vector>
#include <rex/logging.h>

namespace gpu::ui {
namespace {
// The image under the UI: black, 16:9 (the Present keeps its aspect with black bars).
constexpr uint32_t kBackground = 0x5C4EE000;
}  // namespace

Screen::Screen(Layer& layer) : layer_(layer) {}
Screen::~Screen() { Close(); }

bool Screen::Open(void* window, const RenderMode& mode, std::string& evidence) {
  if (dispatcher_) return true;
  auto dispatcher = std::make_unique<Dispatcher>(true);
  // The display's rate without tearing, one frame ahead at most.
  PresentMode present;
  present.allow_tearing = false;
  ClearRequest clear;
  clear.color_keys[0] = kBackground;
  clear.clear_color = true;
  clear.color = {0, 0, 0, 1};
  const auto native_window = platform::WindowHandles(window);
  const bool ready = dispatcher->Initialize(evidence, mode) &&
                     dispatcher->Run([&](Engine& engine, std::string& out) {
                       uint64_t fence = 0;
                       return engine.AttachWindow(native_window, out, present) &&
                              engine.CreateSurface(SurfaceDesc{kBackground, 16, 9}, out) &&
                              engine.ClearSurfaces(clear, fence, out);
                     }, evidence);
  if (!ready) {
    std::string released;
    if (!dispatcher->Shutdown(released)) REXLOG_WARN("[narutorise] UI screen: engine release failed: {}", released);
    return false;
  }
  dispatcher_ = std::move(dispatcher);
  layer_.ForgetEngine();  // A dispatcher at the address of an earlier one is still a new engine.
  stop_ = false;
  thread_ = std::thread([this] { Present(); });
  return true;
}

void Screen::Present() {
  bool failed = false;
  while (!stop_.load()) {
    const auto start = std::chrono::steady_clock::now();
    layer_.Deliver(*dispatcher_);
    std::string evidence;
    const bool presented = dispatcher_->Run([](Engine& engine, std::string& out) {
      uint32_t slot = 0, width = 0, height = 0;
      std::vector<uint8_t> none;
      return engine.StageFront(kBackground, slot, out) && engine.Present(slot, false, none, width, height, out);
    }, evidence);
    if (!presented && !failed) REXLOG_WARN("[narutorise] UI screen: present failed: {}", evidence);
    failed |= !presented;
    // The vsync paces the Presents; a minimized window skips them and does not wait.
    if (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(2))
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
}

void Screen::Close() {
  if (!dispatcher_) return;
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  std::string evidence;
  if (!dispatcher_->Run([](Engine& engine, std::string& out) {
        return engine.ReleaseRemaining(out) && engine.DetachWindow(out);
      }, evidence))
    REXLOG_WARN("[narutorise] UI screen: window release failed: {}", evidence);
  if (!dispatcher_->Shutdown(evidence)) REXLOG_WARN("[narutorise] UI screen: engine release failed: {}", evidence);
  dispatcher_.reset();
  layer_.ForgetEngine();  // The game's engine gets every texture and the frame.
}

}  // namespace gpu::ui
