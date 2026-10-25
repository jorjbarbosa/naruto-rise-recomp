// A screen of the UI before the game runs (the setup screen of the menus): a
// temporary engine on the window that presents the UI layer over black at the
// display's rate (vsync). Its thread delivers the layer and presents, as each
// Swap does in the game, so the layer's frame request clocks the drawing side
// here too. Close stops the thread, releases the swapchain and the engine, and
// has the layer send everything again to the game's engine.
#pragma once

#include "gpu/backend/contracts.h"
#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace gpu {
class Dispatcher;
namespace ui {
class Layer;

class Screen {
 public:
  explicit Screen(Layer& layer);
  ~Screen();
  // window: an HWND. The engine of mode (its API and adapter); nothing stays
  // open when it fails (evidence).
  bool Open(void* window, const RenderMode& mode, std::string& evidence);
  void Close();
  bool IsOpen() const { return dispatcher_ != nullptr; }

 private:
  void Present();
  Layer& layer_;
  std::unique_ptr<Dispatcher> dispatcher_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
};

}  // namespace ui
}  // namespace gpu
