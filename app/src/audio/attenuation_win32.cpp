// The media sessions of the music attenuation on Windows (attenuation.h).
#include "audio/attenuation.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <rex/logging.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>

namespace audio::attenuation {
namespace {
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;

std::mutex mutex;
std::condition_variable wake;
bool watching = false;
std::thread watcher;
std::atomic<bool> playing{false};

bool AnyPlaying(const GlobalSystemMediaTransportControlsSessionManager& manager) {
  for (const auto& session : manager.GetSessions())
    if (session.GetPlaybackInfo().PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing)
      return true;
  return false;
}

void Watch() {
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  {
    GlobalSystemMediaTransportControlsSessionManager manager{nullptr};
    try {
      manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
    } catch (const winrt::hresult_error& error) {
      REXLOG_WARN("[narutorise] Audio: the system's media sessions are not available ({:08X}); no music attenuation",
                  uint32_t(error.code()));
    }
    std::unique_lock lock(mutex);
    while (watching && manager) {
      lock.unlock();
      bool any = false;
      try {
        any = AnyPlaying(manager);
      } catch (const winrt::hresult_error&) {
      }
      if (playing.exchange(any) != any)
        REXLOG_INFO("[narutorise] Audio: {}", any ? "another app plays media; the music fades out"
                                                  : "no other app plays media; the music returns");
      lock.lock();
      wake.wait_for(lock, std::chrono::milliseconds(500), [] { return !watching; });
    }
  }
  playing = false;
  winrt::uninit_apartment();
}
}  // namespace

void Enable(bool on) {
  std::unique_lock lock(mutex);
  if (on == watching) return;
  watching = on;
  if (on) {
    watcher = std::thread(Watch);
    return;
  }
  lock.unlock();
  wake.notify_all();
  if (watcher.joinable()) watcher.join();
}

bool OtherMediaPlaying() { return playing.load(); }
}  // namespace audio::attenuation
