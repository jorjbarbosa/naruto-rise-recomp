#pragma once

// The render driver client of the native audio (XAudioRegisterRenderDriverClient,
// XAudioUnregisterRenderDriverClient, XAudioSubmitRenderDriverFrame).
//
// The title mixes its sound itself and hands the result over one frame at a
// time: it registers a callback once, the callback runs every 256 samples at
// 48 kHz, and on that call the title's mixer submits a frame of 6 channels.
// The Output keeps that clock on its own thread ("Native Audio"), converts
// each submitted frame (mix.h) and puts it into the device (device.h).
//
// Robustness rules (docs/plans, audio nativo):
// - Registering never fails for a callback the title gives, and always fills
//   the handle; the device is not opened there.
// - A device that does not open, or that is lost, becomes the mute device: the
//   callback keeps its clock and the frames are counted and dropped. The device
//   is tried again every `retry`, on a thread of its own: an open can take long
//   to fail (a driver's connect timeout), and the callback's clock never waits
//   for it. Nothing of this reaches the title.
// - A frame address the title gives is checked before it is read; a bad one is
//   counted and dropped.
// - Shutdown stops the callback thread first, then releases the device, then
//   forgets the clients.
//
// The guest side (thread, calls into the title, guest memory, the device
// opener) comes from an OutputHost, so the tests drive the Output without
// the runtime (app/tests/audio_output_test.cpp).
#include "audio/device.h"
#include "audio/mix.h"
#include "audio/pacing.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace narutorise::audio {
// The title only stores the handle, tests it against zero and passes it back
// (sub_82149870): any nonzero value works. "NR" and the client's slot.
constexpr uint32_t kHandleTag = 0x4E520000;
constexpr uint32_t kMaximumClients = 8;
// The handle of a client past kMaximumClients: accepted everywhere, never called.
constexpr uint32_t kOverflowHandle = kHandleTag | 0xFFFF;
// The queue is watched for gaps after this many callbacks (about 1 s).
constexpr uint64_t kWarmCallbacks = 200;

class OutputHost {
 public:
  virtual ~OutputHost() = default;
  // Starts `body` on the callback thread.
  virtual bool StartThread(std::function<void()> body) = 0;
  // Waits for the body to return. False when it did not in time.
  virtual bool JoinThread(std::chrono::milliseconds timeout) = 0;
  // Runs the title's callback with the argument it registered, on the
  // callback thread. (The title's callback, sub_8215AEE8, ignores it.)
  virtual void CallClient(uint32_t callback, uint32_t argument) = 0;
  // The host address of [address, address + kFrameBytes) when the whole range
  // is committed and readable guest memory; null otherwise.
  virtual const uint8_t* MapFrame(uint32_t address) = 0;
  virtual std::unique_ptr<Device> OpenDevice(std::string& error) = 0;
  virtual MixSettings Mix() = 0;
  // A fact worth a log line: device open, failure, loss, first register.
  virtual void Note(std::string_view event, std::string_view detail) = 0;
  // Waits until `deadline` (sub-millisecond on Windows).
  virtual void SleepUntil(std::chrono::steady_clock::time_point deadline) = 0;
  // The samples just put into the device (a diagnostic capture may keep them).
  virtual void Captured(const float* samples, int channels) {
    (void)samples;
    (void)channels;
  }
};

struct OutputOptions {
  std::chrono::milliseconds retry{5000};
  std::chrono::milliseconds stall{1000};
  std::chrono::milliseconds join{1000};
};

struct OutputCounters {
  uint64_t registers = 0, unregisters = 0, register_refused = 0, register_overflow = 0;
  uint64_t unregister_unknown = 0;
  uint32_t first_callback = 0, first_argument = 0;
  uint64_t callbacks = 0, periods = 0, double_periods = 0, skipped_periods = 0, grid_restarts = 0;
  // After the first kWarmCallbacks: periods that found the device's queue
  // empty (the device ran dry: a gap), and the smallest queue seen.
  uint64_t queue_empty_periods = 0;
  int32_t queue_min = -1;
  // Of those, the ones after MarkClosing: the title winds down at the close
  // and stops submitting, which is not a gap in play.
  uint64_t queue_empty_closing = 0;
  bool closing = false;
  uint64_t submits = 0, frames_to_device = 0, frames_muted = 0, frames_invalid = 0, submit_unknown = 0;
  uint64_t device_opens = 0, open_failures = 0, device_lost = 0, device_stalls = 0;
  std::string last_open_error;
  DeviceFacts device;
  LayoutCheck layout;
  ChannelUse channels;
  bool thread_started = false, thread_start_failed = false, thread_joined = false, shut_down = false;
};

class Output {
 public:
  Output(OutputHost& host, OutputOptions options = {});
  ~Output();
  Output(const Output&) = delete;
  Output& operator=(const Output&) = delete;

  // XAudioRegisterRenderDriverClient: the handle, or 0 for a null callback
  // (the title's own error, refused as the console refuses it).
  uint32_t Register(uint32_t callback, uint32_t argument);
  void Unregister(uint32_t handle);
  // XAudioSubmitRenderDriverFrame, from whichever title thread mixes.
  void Submit(uint32_t handle, uint32_t samples);
  // The window is closing (before the renderer winds the title down).
  void MarkClosing();
  void Shutdown();
  OutputCounters Counters() const;

 private:
  struct Client {
    bool in_use = false;
    uint32_t callback = 0, argument = 0;
    uint64_t generation = 0;  // A new registration resets the slot's pacer.
  };
  // A retry's open, filled by the opener thread and taken by the callback
  // thread. The opener touches only this and the host, never the Output.
  struct PendingOpen {
    std::mutex mutex;
    std::condition_variable done_signal;
    bool done = false;
    std::unique_ptr<Device> device;
    std::string error;
  };
  void Run();
  // Puts an opened device in place, or counts and notes the failure.
  void Opened(std::unique_ptr<Device> device, std::string error, bool first);
  // The callback thread's: starts a retry's open, and takes its result.
  void StartRetry();
  void CollectRetry();
  // Shutdown, once the callback thread is joined: waits up to `join` for a
  // retry in flight and closes what it opened.
  void StopRetry();
  // Replaces a failed device with the mute one. Under device_mutex_.
  void LoseDevice(std::string_view reason);
  void Pump(size_t index, std::chrono::steady_clock::time_point now);

  OutputHost& host_;
  const OutputOptions options_;
  // The callback thread's own (Shutdown's once that thread is joined).
  std::thread opener_;
  std::shared_ptr<PendingOpen> pending_;  // A retry in flight.

  mutable std::mutex clients_mutex_;
  std::array<Client, kMaximumClients> clients_{};
  uint64_t generation_ = 0;
  // The callback thread's own.
  std::array<Pacer, kMaximumClients> pacers_;
  std::array<uint64_t, kMaximumClients> paced_generation_{};
  bool reset_pacers_ = false;  // Under device_mutex_: a new device was opened.
  bool thread_started_ = false;
  bool thread_start_failed_ = false;
  bool shut_down_ = false;
  bool thread_joined_ = false;

  mutable std::mutex device_mutex_;
  std::unique_ptr<Device> device_;
  bool device_failed_ = false;  // The mute device stands in for a failed one.
  std::chrono::steady_clock::time_point retry_at_{};
  std::vector<float> mixed_;
  LayoutCheck layout_;
  ChannelUse channel_use_;
  DeviceFacts device_facts_;
  std::string last_open_error_;

  std::atomic<bool> running_{false};
  std::atomic<uint64_t> registers_{0}, unregisters_{0}, register_refused_{0}, register_overflow_{0},
      unregister_unknown_{0}, callbacks_{0}, periods_{0}, double_periods_{0}, skipped_periods_{0},
      grid_restarts_{0}, submits_{0}, frames_to_device_{0}, frames_muted_{0}, frames_invalid_{0},
      submit_unknown_{0}, device_opens_{0}, open_failures_{0}, device_lost_{0}, device_stalls_{0},
      queue_empty_periods_{0}, queue_empty_closing_{0};
  std::atomic<bool> closing_{false};
  std::atomic<int32_t> queue_min_{-1};
  std::atomic<uint32_t> first_callback_{0}, first_argument_{0};
};
}  // namespace narutorise::audio
