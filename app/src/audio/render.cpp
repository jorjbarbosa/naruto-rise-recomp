// The output path of the native audio layer (render.h).
#include "audio/render.h"
#include "audio/device.h"
#include "audio/diagnostics.h"
#include "audio/mix.h"
#include "audio/pacing.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <rex/audio/downmix.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_STRING(audio_layer, "native", "narutorise/Audio",
                      "Audio output: native (the port's own, paced for every device) or sdk (the SDK's)")
    .allowed({"native", "sdk"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(audio_channels, "stereo", "narutorise/Audio",
                      "Channels of the native audio output: stereo, or surround for 5.1")
    .allowed({"stereo", "surround"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(master_volume, 1.0, "narutorise/Audio", "Master volume, 0 to 1")
    .range(0.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace audio {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto kRetryEvery = std::chrono::seconds(1);
constexpr auto kReportEvery = std::chrono::minutes(1);
constexpr auto kStopWait = std::chrono::milliseconds(500);

// The requested options, from the thread of the settings.
std::atomic<bool> native_requested{true}, surround_requested{false};
std::atomic<float> gain{1.0f};

std::mutex client_mutex;
Client client;
Owner owner = Owner::kNone;

// The device is the render thread's, but a callback of the SDK's worker in
// flight while the client moves to the native layer delivers there too.
std::mutex device_mutex;
Device device;
bool fed = false;  // A frame went into the device since it opened.

std::atomic<bool> running{false}, stopped{false};
bool started = false;  // Under client_mutex.
rex::system::object_ref<rex::system::XHostThread> thread;
// The callback's argument: a guest word holding the context, as the SDK passes it.
std::atomic<uint32_t> context_word{0};

// Sleeps until an absolute time with sub-millisecond precision: Sleep on
// Windows rounds to the millisecond and to the system timer period.
class FrameClock {
 public:
  FrameClock() {
#ifdef _WIN32
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer_) timer_ = CreateWaitableTimerW(nullptr, TRUE, nullptr);
#endif
  }
  ~FrameClock() {
#ifdef _WIN32
    if (timer_) CloseHandle(timer_);
#endif
  }
  FrameClock(const FrameClock&) = delete;
  FrameClock& operator=(const FrameClock&) = delete;
  void SleepUntil(Clock::time_point deadline) {
    const auto remaining = deadline - Clock::now();
    if (remaining <= Clock::duration::zero()) return;
#ifdef _WIN32
    LARGE_INTEGER due;
    due.QuadPart = -std::max<LONGLONG>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100);
    if (timer_ && SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(timer_, INFINITE);
      return;
    }
#endif
    std::this_thread::sleep_until(deadline);
  }

 private:
#ifdef _WIN32
  HANDLE timer_ = nullptr;
#endif
};

const char* OwnerName(Owner value) {
  return value == Owner::kNative ? "native" : value == Owner::kSdk ? "SDK" : "no";
}

// At a period boundary: moves the client to the requested layer. Returns the
// client the grid serves, none while the SDK holds it.
Client Serve() {
  Client current;
  Owner held;
  {
    std::lock_guard lock(client_mutex);
    current = client;
    held = owner;
  }
  if (!current.callback) return {};
  const Owner wanted = native_requested.load() ? Owner::kNative : Owner::kSdk;
  if (held != wanted) {
    held = MoveSdkClient(current, wanted == Owner::kSdk) ? Owner::kSdk : Owner::kNative;
    {
      std::lock_guard lock(client_mutex);
      // Unless the title registered again in between.
      if (client.driver == current.driver && client.callback == current.callback) owner = held;
    }
    if (held == wanted)
      REXLOG_INFO("[narutorise] Audio: the title's output moved to the {} layer", OwnerName(held));
    else
      REXLOG_WARN("[narutorise] Audio: the title's output stays in the {} layer", OwnerName(held));
  }
  return held == Owner::kNative ? current : Client{};
}

// The device in the requested layout, (re)opened at most once a second.
// Returns its queue, -1 without a device; starved when it ran empty after a frame.
int32_t PrepareDevice(Pacer& pacer, Clock::time_point now, Clock::time_point& retry, bool& reported, bool& starved) {
  const Layout wanted = surround_requested.load() ? Layout::kSurround : Layout::kStereo;
  std::lock_guard lock(device_mutex);
  starved = fed && device.Queued() == 0;
  if (device.open() && device.layout() != wanted) device.Close();
  if (!device.open() && now >= retry) {
    retry = now + kRetryEvery;
    pacer.Reset();
    fed = false;
    if (device.Open(wanted)) {
      pacer.SetDeviceBlock(device.block());
      REXLOG_INFO("[narutorise] Audio device {}", device.description());
      reported = false;
    } else if (!reported) {
      // The title keeps its rhythm without sound; once in the log.
      REXLOG_WARN("[narutorise] Audio: {}; the game runs without sound until a device appears",
                  device.description());
      reported = true;
    }
  }
  return device.Queued();
}

void CloseDevice() {
  std::lock_guard lock(device_mutex);
  device.Close();
}

int32_t Queued() {
  std::lock_guard lock(device_mutex);
  return device.Queued();
}

int Run() {
  auto* kernel = rex::system::kernel_state();
  auto* dispatcher = kernel->function_dispatcher();
  auto* state = rex::runtime::ThreadState::Get();
  auto& counters = diagnostics::Get();
  FrameClock clock;
  Grid grid(Clock::now());
  Pacer pacer;
  bool reported = false, stalled = false;
  auto retry = grid.next(), report = grid.next() + kReportEvery;
  while (running.load()) {
    clock.SleepUntil(grid.next());
    const auto now = Clock::now();
    if (!grid.Advance(now)) ++counters.restarts;
    if (!running.load()) break;
    const Client served = Serve();
    if (!served.callback) {
      CloseDevice();
      continue;
    }
    ++counters.periods;
    bool starved = false;
    const int32_t queued = PrepareDevice(pacer, now, retry, reported, starved);
    if (starved) ++counters.underruns;
    const int callbacks = pacer.Callbacks(queued);
    if (pacer.Stalled() != stalled) {
      stalled = pacer.Stalled();
      if (stalled) {
        ++counters.stalls;
        REXLOG_WARN("[narutorise] Audio: the device took no sample for 250 ms; the game keeps its rhythm");
      }
    }
    // A stalled device's queue does not grow without bound.
    if (stalled && queued > 2 * pacer.Limit()) {
      std::lock_guard lock(device_mutex);
      device.Clear();
    }
    if (callbacks == 0) ++counters.skipped;
    if (callbacks == 2) ++counters.doubled;
    uint64_t args[] = {context_word.load()};
    for (int i = 0; i < callbacks && running.load(); ++i) {
      dispatcher->Execute(state, served.callback, args, 1);
      ++counters.callbacks;
    }
    pacer.Ran(Queued());
    counters.largest_block = pacer.largest_block();
    counters.queue_limit = pacer.Limit();
    if (now >= report) {
      diagnostics::Report("running");
      report = now + kReportEvery;
    }
  }
  return 0;
}

void Start() {
  auto* kernel = rex::system::kernel_state();
  running = true;
  thread = rex::system::object_ref<rex::system::XHostThread>(
      new rex::system::XHostThread(kernel, 128 * 1024, 0, [] { return Run(); }));
  thread->set_name("Native Audio Render");
  if (thread->Create() != 0) {
    running = false;
    REXLOG_ERROR("[narutorise] Audio: the render thread could not start; the game runs without sound");
  }
}
}  // namespace

void Initialize() { ReloadSettings(); }

void ReloadSettings() {
  const bool native = REXCVAR_GET(audio_layer) != "sdk";
  const bool surround = REXCVAR_GET(audio_channels) == "surround";
  const double volume = std::clamp(REXCVAR_GET(master_volume), 0.0, 1.0);
  native_requested = native;
  surround_requested = surround;
  gain = VolumeGain(volume);
  rex::audio::SetOutputGain(VolumeGain(volume));  // The SDK layer's output.
  REXLOG_INFO("[narutorise] Audio settings: {} layer, {}, master volume {:.2f}", native ? "native" : "SDK",
              surround ? "5.1" : "stereo", volume);
}

bool NativeRequested() { return native_requested.load(); }

void Register(const Client& next, Owner held) {
  bool start = false;
  {
    std::lock_guard lock(client_mutex);
    client = next;
    owner = next.callback ? held : Owner::kNone;
    start = next.callback && !started && !stopped.load();
    started |= start;
  }
  REXLOG_INFO("[narutorise] Audio: the title {} its output (driver {:08X}, callback {:08X}, context {:08X}) "
              "in the {} layer", next.callback ? "registered" : "released", next.driver, next.callback,
              next.context, OwnerName(held));
  auto* memory = rex::system::kernel_state()->memory();
  if (start) context_word = memory->SystemHeapAlloc(4);
  if (const uint32_t word = context_word.load(); word && next.callback) {
    const uint32_t value = std::byteswap(next.context);
    std::memcpy(memory->TranslateVirtual<uint8_t*>(word), &value, 4);
  }
  if (start) Start();
}

void Deliver(const uint8_t* frame) {
  diagnostics::Record(frame);
  ++diagnostics::Get().frames;
  std::array<float, kFrameSamples * kFrameChannels> mixed;
  std::lock_guard lock(device_mutex);
  if (!device.open()) return;
  Mix(frame, device.layout(), gain.load(), mixed.data());
  if (!device.Put(mixed.data(), kFrameSamples * Channels(device.layout()))) {
    REXLOG_WARN("[narutorise] Audio: the device refused a frame; reopening");
    device.Close();
    return;
  }
  fed = true;
}

void Shutdown() {
  if (stopped.exchange(true)) return;
  running = false;
  if (thread) {
    auto* host = thread->thread();
    const bool exited = host && rex::thread::Wait(host, false, kStopWait) == rex::thread::WaitResult::kSuccess;
    if (!exited)
      REXLOG_WARN("[narutorise] Audio: the render thread did not stop within {} ms", kStopWait.count());
  }
  {
    std::lock_guard lock(device_mutex);
    device.Release();
  }
  diagnostics::Report("end");
  diagnostics::CloseRecording();
}
}  // namespace audio
