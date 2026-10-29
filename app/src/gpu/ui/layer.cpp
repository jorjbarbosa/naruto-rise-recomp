// The UI layer of the native Present (layer.h).
#include "gpu/ui/layer.h"
#include "gpu/backend/dispatcher.h"
#include <atomic>
#include <string>
#include <rex/logging.h>

namespace gpu::ui {
namespace {
// The first failure of each UI operation goes to the log; the operation
// reports success, so the dispatcher latches nothing.
bool Logged(std::atomic<bool>& once, const char* operation, const std::string& evidence) {
  if (!once.exchange(true)) REXLOG_WARN("[narutorise] UI layer: {} failed: {}", operation, evidence);
  return true;
}
std::atomic<bool> upload_failed{false}, release_failed{false}, frame_failed{false};
}  // namespace

uint32_t Layer::CreateTexture(uint32_t width, uint32_t height, std::vector<uint8_t> rgba) {
  std::lock_guard lock(mutex_);
  const uint32_t key = next_key_++;
  textures_[key] = {width, height, std::make_shared<const std::vector<uint8_t>>(std::move(rgba))};
  return key;
}

void Layer::ReleaseTexture(uint32_t key) {
  std::lock_guard lock(mutex_);
  auto it = textures_.find(key);
  if (it == textures_.end()) return;
  if (it->second.delivered) released_.push_back(key);
  textures_.erase(it);
}

void Layer::SetFrame(UiFrame frame) {
  auto shared = std::make_shared<const UiFrame>(std::move(frame));
  std::lock_guard lock(mutex_);
  frame_ = std::move(shared);
  frame_delivered_ = false;
}

void Layer::SetFrameRequest(std::function<void()> request) {
  std::lock_guard lock(mutex_);
  request_ = std::move(request);
}

void Layer::SetFirstPresentCallback(std::function<void()> callback) {
  std::lock_guard lock(mutex_);
  first_present_ = std::move(callback);
  first_present_pending_.store(bool(first_present_), std::memory_order_release);
}

void Layer::NotifyPresented(bool successful, bool presented) {
  if (!successful || !presented || !first_present_pending_.load(std::memory_order_acquire)) return;
  std::function<void()> callback;
  {
    std::lock_guard lock(mutex_);
    callback = std::move(first_present_);
    first_present_pending_.store(false, std::memory_order_release);
  }
  if (callback) callback();
}

void Layer::ForgetEngine() {
  std::lock_guard lock(mutex_);
  dispatcher_ = nullptr;
}

bool Layer::Deliver(Dispatcher& dispatcher) {
  struct Upload {
    uint32_t key, width, height;
    std::shared_ptr<const std::vector<uint8_t>> rgba;
  };
  std::vector<Upload> uploads;
  std::vector<uint32_t> releases;
  std::shared_ptr<const UiFrame> frame;
  std::function<void()> request;
  {
    std::lock_guard lock(mutex_);
    if (dispatcher_ != &dispatcher) {  // A new engine has nothing of the layer.
      dispatcher_ = &dispatcher;
      for (auto& [key, texture] : textures_) texture.delivered = false;
      released_.clear();
      frame_delivered_ = !frame_;
    }
    for (auto& [key, texture] : textures_)
      if (!texture.delivered) {
        uploads.push_back({key, texture.width, texture.height, texture.rgba});
        texture.delivered = true;
      }
    releases.swap(released_);
    if (!frame_delivered_) frame = frame_;
    frame_delivered_ = true;
    request = request_;
  }
  bool posted = true;
  for (auto& u : uploads)
    posted = posted && dispatcher.Post([u](Engine& engine, std::string& out) {
      return engine.UploadUiTexture(u.key, u.width, u.height, *u.rgba, out) || Logged(upload_failed, "upload", out);
    });
  for (uint32_t key : releases)
    posted = posted && dispatcher.Post([key](Engine& engine, std::string& out) {
      return engine.ReleaseUiTexture(key, out) || Logged(release_failed, "release", out);
    });
  if (frame)
    posted = posted && dispatcher.Post([frame](Engine& engine, std::string& out) {
      return engine.SetUiFrame(*frame, out) || Logged(frame_failed, "frame", out);
    });
  if (request) request();
  return posted;
}

Layer& UiLayer() {
  static Layer layer;
  return layer;
}

}  // namespace gpu::ui
