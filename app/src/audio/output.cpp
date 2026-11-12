// The render driver client of the native audio (output.h).
#include "audio/output.h"

#include <fmt/format.h>

namespace narutorise::audio {
namespace {
using Clock = std::chrono::steady_clock;

std::string FactsJson(const DeviceFacts& facts) {
  return fmt::format("\"kind\":\"{}\",\"driver\":\"{}\",\"endpoint_channels\":{},\"endpoint_frequency\":{},"
                     "\"block_samples\":{},\"channels\":{}",
                     facts.kind, facts.driver, facts.endpoint_channels, facts.endpoint_frequency,
                     facts.block_samples, facts.channels);
}
}  // namespace

Output::Output(OutputHost& host, OutputOptions options) : host_(host), options_(options) {
  device_ = MuteDevice();
  device_facts_ = device_->Facts();
}

Output::~Output() { Shutdown(); }

uint32_t Output::Register(uint32_t callback, uint32_t argument) {
  if (!callback) {
    register_refused_.fetch_add(1);
    host_.Note("register-refused", "a null callback");
    return 0;
  }
  std::lock_guard lock(clients_mutex_);
  registers_.fetch_add(1);
  if (registers_.load() == 1) {
    first_callback_.store(callback);
    first_argument_.store(argument);
  }
  if (shut_down_) return kOverflowHandle;
  size_t index = kMaximumClients;
  for (size_t i = 0; i < kMaximumClients; ++i) {
    if (!clients_[i].in_use) {
      index = i;
      break;
    }
  }
  if (index == kMaximumClients) {
    register_overflow_.fetch_add(1);
    host_.Note("register-overflow", fmt::format("callback 0x{:08X}", callback));
    return kOverflowHandle;
  }
  clients_[index] = {true, callback, argument, ++generation_};
  host_.Note("register", fmt::format("client {}: callback 0x{:08X}, argument 0x{:08X}", index, callback, argument));
  if (!thread_started_ && !thread_start_failed_) {
    running_.store(true);
    if (host_.StartThread([this] { Run(); })) {
      thread_started_ = true;
    } else {
      running_.store(false);
      thread_start_failed_ = true;
      host_.Note("thread-start-failed", "the callback will not run");
    }
  }
  return kHandleTag | static_cast<uint32_t>(index);
}

void Output::Unregister(uint32_t handle) {
  std::lock_guard lock(clients_mutex_);
  if (handle == kOverflowHandle) {
    unregisters_.fetch_add(1);
    return;
  }
  const uint32_t index = handle & 0xFFFF;
  if ((handle & 0xFFFF0000) != kHandleTag || index >= kMaximumClients || !clients_[index].in_use) {
    unregister_unknown_.fetch_add(1);
    return;
  }
  clients_[index] = {};
  unregisters_.fetch_add(1);
}

void Output::Submit(uint32_t handle, uint32_t samples) {
  submits_.fetch_add(1);
  if (handle == kOverflowHandle) {
    frames_muted_.fetch_add(1);
    return;
  }
  const uint32_t index = handle & 0xFFFF;
  {
    std::lock_guard lock(clients_mutex_);
    if ((handle & 0xFFFF0000) != kHandleTag || index >= kMaximumClients || !clients_[index].in_use) {
      submit_unknown_.fetch_add(1);
      return;
    }
  }
  const uint8_t* frame = host_.MapFrame(samples);
  if (!frame) {
    frames_invalid_.fetch_add(1);
    return;
  }
  const MixSettings mix = host_.Mix();
  std::lock_guard lock(device_mutex_);
  layout_.Add(frame);
  channel_use_.Add(frame, mix.stereo);
  const int channels = device_ ? device_->Channels() : 0;
  if (channels == 0) {
    frames_muted_.fetch_add(1);
    return;
  }
  mixed_.resize(size_t(kFrameSamples) * channels);
  MixFrame(frame, mixed_.data(), channels, mix);
  if (!device_->Put(mixed_.data())) {
    LoseDevice("the device refused a frame");
    frames_muted_.fetch_add(1);
    return;
  }
  frames_to_device_.fetch_add(1);
  host_.Captured(mixed_.data(), channels);
}

void Output::LoseDevice(std::string_view reason) {
  device_lost_.fetch_add(1);
  host_.Note("device-lost", fmt::format("{}; {}", reason, FactsJson(device_facts_)));
  device_ = MuteDevice();
  device_facts_ = device_->Facts();
  device_failed_ = true;
  retry_at_ = Clock::now() + options_.retry;
}

void Output::OpenDevice(bool first) {
  std::string error;
  std::unique_ptr<Device> device = host_.OpenDevice(error);
  std::lock_guard lock(device_mutex_);
  if (!running_.load()) return;  // Shut down while opening: leave it closed.
  if (device) {
    device_ = std::move(device);
    device_facts_ = device_->Facts();
    device_failed_ = false;
    reset_pacers_ = true;
    device_opens_.fetch_add(1);
    host_.Note(first ? "device-open" : "device-reopen", FactsJson(device_facts_));
    return;
  }
  const uint64_t failures = open_failures_.fetch_add(1) + 1;
  last_open_error_ = error.empty() ? "unknown" : error;
  device_failed_ = true;
  retry_at_ = Clock::now() + options_.retry;
  // The first failure and every hundredth (one each ~8 minutes) are logged.
  if (first || failures == 1 || failures % 100 == 0)
    host_.Note("device-open-failed", fmt::format("{} (failure {}; mute until a retry opens it)", last_open_error_,
                                                 failures));
}

void Output::Pump(size_t index, Clock::time_point now) {
  uint32_t callback = 0, argument = 0;
  uint64_t generation = 0;
  {
    std::lock_guard lock(clients_mutex_);
    const Client& client = clients_[index];
    if (!client.in_use) return;
    callback = client.callback;
    argument = client.argument;
    generation = client.generation;
  }
  Pacer& pacer = pacers_[index];
  if (paced_generation_[index] != generation) {
    paced_generation_[index] = generation;
    pacer.Reset();
  }
  int32_t queued;
  {
    std::lock_guard lock(device_mutex_);
    if (!device_) return;
    queued = device_->Queued();
  }
  if (queued >= 0 && callbacks_.load() > kWarmCallbacks) {
    if (queued == 0) {
      // The first few go to the log with their time, to tell a gap in play
      // from one at a load or at the close.
      const uint64_t empties = queue_empty_periods_.fetch_add(1) + 1;
      if (closing_.load()) queue_empty_closing_.fetch_add(1);
      if (empties <= 8 || empties % 100 == 0)
        host_.Note("queue-empty", fmt::format("period {} after {} callbacks (largest take {}, target {})", empties,
                                              callbacks_.load(), pacer.LargestTake(), pacer.Target()));
    }
    const int32_t least = queue_min_.load();
    if (least < 0 || queued < least) queue_min_.store(queued);
  }
  const int callbacks = pacer.Callbacks(queued);
  if (callbacks == 0) {
    skipped_periods_.fetch_add(1);
    if (pacer.Stalled(now, options_.stall)) {
      std::lock_guard lock(device_mutex_);
      device_stalls_.fetch_add(1);
      LoseDevice("the device stopped taking frames");
      pacer.Reset();
    }
    return;
  }
  periods_.fetch_add(1);
  if (callbacks > 1) double_periods_.fetch_add(1);
  for (int i = 0; i < callbacks && running_.load(); ++i) {
    host_.CallClient(callback, argument);
    callbacks_.fetch_add(1);
  }
  std::lock_guard lock(device_mutex_);
  if (device_) pacer.After(device_->Queued());
}

void Output::Run() {
  OpenDevice(true);
  Grid grid(Clock::now());
  while (running_.load()) {
    host_.SleepUntil(grid.Next());
    if (!running_.load()) break;
    const auto now = Clock::now();
    if (grid.Advance(now)) grid_restarts_.fetch_add(1);
    bool retry = false;
    {
      std::lock_guard lock(device_mutex_);
      retry = device_failed_ && now >= retry_at_;
      if (reset_pacers_) {
        reset_pacers_ = false;
        for (auto& pacer : pacers_) pacer.Reset();
      }
    }
    if (retry) OpenDevice(false);
    for (size_t index = 0; index < kMaximumClients && running_.load(); ++index) Pump(index, now);
  }
}

void Output::MarkClosing() { closing_.store(true); }

void Output::Shutdown() {
  bool started;
  {
    std::lock_guard lock(clients_mutex_);
    if (shut_down_) return;
    shut_down_ = true;
    started = thread_started_;
  }
  // 1. The callback thread.
  running_.store(false);
  const bool joined = !started || host_.JoinThread(options_.join);
  // 2. The device: a straggling Submit finds none and drops its frame.
  {
    std::lock_guard lock(device_mutex_);
    device_.reset();
  }
  // 3. The clients.
  std::lock_guard lock(clients_mutex_);
  thread_joined_ = joined;
  if (!joined) host_.Note("shutdown", "the callback thread did not stop in time");
  for (auto& client : clients_) client = {};
}

OutputCounters Output::Counters() const {
  OutputCounters c;
  c.registers = registers_.load();
  c.unregisters = unregisters_.load();
  c.register_refused = register_refused_.load();
  c.register_overflow = register_overflow_.load();
  c.unregister_unknown = unregister_unknown_.load();
  c.first_callback = first_callback_.load();
  c.first_argument = first_argument_.load();
  c.callbacks = callbacks_.load();
  c.periods = periods_.load();
  c.double_periods = double_periods_.load();
  c.skipped_periods = skipped_periods_.load();
  c.grid_restarts = grid_restarts_.load();
  c.submits = submits_.load();
  c.frames_to_device = frames_to_device_.load();
  c.frames_muted = frames_muted_.load();
  c.frames_invalid = frames_invalid_.load();
  c.submit_unknown = submit_unknown_.load();
  c.device_opens = device_opens_.load();
  c.open_failures = open_failures_.load();
  c.device_lost = device_lost_.load();
  c.device_stalls = device_stalls_.load();
  c.queue_empty_periods = queue_empty_periods_.load();
  c.queue_min = queue_min_.load();
  c.queue_empty_closing = queue_empty_closing_.load();
  c.closing = closing_.load();
  {
    std::lock_guard lock(device_mutex_);
    c.last_open_error = last_open_error_;
    c.device = device_facts_;
    c.layout = layout_;
    c.channels = channel_use_;
  }
  std::lock_guard lock(clients_mutex_);
  c.thread_started = thread_started_;
  c.thread_start_failed = thread_start_failed_;
  c.thread_joined = thread_joined_;
  c.shut_down = shut_down_;
  return c;
}
}  // namespace narutorise::audio
