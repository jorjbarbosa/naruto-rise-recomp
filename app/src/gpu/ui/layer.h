// The UI layer of the native Present (UiFrame, backend/contracts.h) between
// the thread that draws the UI and the engine owner. The drawing side (the
// SDK's UI thread) creates and releases RGBA8 textures and sets frames here,
// under a lock, without reaching the engine; each Swap delivers what changed
// to the dispatcher ahead of its Present (Post is FIFO), from the threads of
// the session. Every live texture stays here as a CPU copy, so an engine that
// comes later gets all of them before the first frame it draws. A failure of
// the UI is logged, never latched: the game goes on without its UI.
#pragma once

#include "gpu/backend/contracts.h"
#include <cstdint>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace gpu {
class Dispatcher;
namespace ui {

class Layer {
 public:
  // Any thread. A texture's key is never 0 (the engine's white texture) and
  // never reused; rgba holds width * height RGBA8 texels, top row first.
  uint32_t CreateTexture(uint32_t width, uint32_t height, std::vector<uint8_t> rgba);
  void ReleaseTexture(uint32_t key);
  // The frame every Present draws from the next delivery on; empty: nothing.
  void SetFrame(UiFrame frame);
  // Runs at every delivery, after it, on the Swap's thread: the drawing side
  // asks there for its next frame (the Swaps are the clock of the layer). It
  // must not block. Empty: none.
  void SetFrameRequest(std::function<void()> request);
  // One-shot confirmation for the game's first successful, non-skipped
  // Present. The engine owner notifies after Present returns; setup's screen
  // never notifies this callback. Clearing it cancels a pending confirmation.
  void SetFirstPresentCallback(std::function<void()> callback);
  void NotifyPresented(bool successful, bool presented);
  // The Swap's thread, before it posts its Present: the textures created and
  // released and the frame set since the last delivery, in that order (to a
  // dispatcher not delivered to before, every live texture and the frame).
  // False when the dispatcher refused a post (it holds a failure).
  bool Deliver(Dispatcher& dispatcher);
  // The engine of the last delivery is gone: the next one sends everything.
  void ForgetEngine();

 private:
  struct Texture {
    uint32_t width = 0, height = 0;
    std::shared_ptr<const std::vector<uint8_t>> rgba;
    bool delivered = false;
  };
  std::mutex mutex_;
  std::unordered_map<uint32_t, Texture> textures_;
  std::vector<uint32_t> released_;  // Delivered, released since the last delivery.
  std::shared_ptr<const UiFrame> frame_;
  bool frame_delivered_ = true;
  std::function<void()> request_;
  std::function<void()> first_present_;
  std::atomic<bool> first_present_pending_{false};
  const Dispatcher* dispatcher_ = nullptr;  // Of the last delivery.
  uint32_t next_key_ = 1;
};

// The layer of the process: the native Present delivers it (d3d/present.cpp).
Layer& UiLayer();

}  // namespace ui
}  // namespace gpu
