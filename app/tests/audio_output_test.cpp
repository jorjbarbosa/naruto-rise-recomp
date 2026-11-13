// The render driver client of the native audio (audio/output.h), with a fake
// guest and fake devices: registering never fails, a device that does not
// open or is lost becomes the mute device and is tried again (beside the
// callback, which keeps its clock while a retry takes long), bad addresses
// and handles are counted, and shutdown goes thread, device, clients.
#include "audio/output.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace narutorise::audio;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {
int checks = 0;
void Check(bool value, const std::string& message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

constexpr uint32_t kCallback = 0x8215AEE8, kArgument = 0x40001234, kFrameAddress = 0x1000;

struct Events {
  std::mutex mutex;
  std::vector<std::string> list;
  void Add(std::string event) {
    std::lock_guard lock(mutex);
    list.push_back(std::move(event));
  }
  std::vector<std::string> Copy() {
    std::lock_guard lock(mutex);
    return list;
  }
  long Index(const std::string& event) {
    const auto copy = Copy();
    const auto it = std::find(copy.begin(), copy.end(), event);
    return it == copy.end() ? -1 : long(it - copy.begin());
  }
};

// Takes 48 kHz of samples on the wall clock, as a device does.
class FakeDevice final : public Device {
 public:
  FakeDevice(Events& events, int channels, bool fail_put, bool consume)
      : events_(events), fail_put_(fail_put), consume_(consume), start_(Clock::now()) {
    facts_.kind = "fake";
    facts_.name = "Fake \"device\"";
    facts_.channels = channels;
    facts_.endpoint_channels = channels;
  }
  ~FakeDevice() override { events_.Add("device-closed"); }
  const DeviceFacts& Facts() const override { return facts_; }
  bool Put(const float* samples) override {
    if (fail_put_) return false;
    std::lock_guard lock(mutex_);
    put_ += kFrameSamples;
    if (first_.empty()) first_.assign(samples, samples + kFrameSamples * facts_.channels);
    return true;
  }
  int32_t Queued() override {
    std::lock_guard lock(mutex_);
    const int64_t taken = consume_ ? std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start_).count() * 48 / 1000 : 0;
    return static_cast<int32_t>(std::max<int64_t>(0, put_ - taken));
  }
  std::vector<float> First() {
    std::lock_guard lock(mutex_);
    return first_;
  }

 private:
  Events& events_;
  DeviceFacts facts_;
  bool fail_put_, consume_;
  Clock::time_point start_;
  std::mutex mutex_;
  int64_t put_ = 0;
  std::vector<float> first_;
};

class FakeHost final : public OutputHost {
 public:
  Events events;
  std::atomic<int> opens_failing{0};  // The next opens that fail...
  std::chrono::milliseconds fail_delay{0};  // ...each after this long.
  std::atomic<int> opens{0};
  bool fail_put = false;
  bool consume = true;
  int channels = 2;
  std::atomic<FakeDevice*> device{nullptr};
  std::atomic<uint64_t> calls{0};
  std::atomic<uint32_t> last_argument{0};
  Output* output = nullptr;
  std::atomic<uint32_t> submit_handle{0};
  std::vector<uint8_t> frame = std::vector<uint8_t>(kFrameBytes);

  FakeHost() {
    // A recognizable frame: front left 0.25 everywhere (big-endian).
    const uint32_t bits = std::byteswap(std::bit_cast<uint32_t>(0.25f));
    for (uint32_t i = 0; i < kFrameSamples; ++i) std::memcpy(frame.data() + i * 4, &bits, 4);
  }
  ~FakeHost() override {
    if (thread_.joinable()) thread_.join();
  }
  bool StartThread(std::function<void()> body) override {
    thread_ = std::thread([this, body = std::move(body)] {
      body();
      events.Add("thread-exit");
      std::lock_guard lock(done_mutex_);
      done_ = true;
      done_signal_.notify_all();
    });
    return true;
  }
  bool JoinThread(std::chrono::milliseconds timeout) override {
    std::unique_lock lock(done_mutex_);
    if (!done_signal_.wait_for(lock, timeout, [this] { return done_; })) return false;
    lock.unlock();
    thread_.join();
    return true;
  }
  void CallClient(uint32_t callback, uint32_t argument) override {
    Check(callback == kCallback, "The registered callback is called");
    last_argument.store(argument);
    calls.fetch_add(1);
    // The title's mixer submits its frame from inside the callback.
    if (output && submit_handle.load()) output->Submit(submit_handle.load(), kFrameAddress);
  }
  const uint8_t* MapFrame(uint32_t address) override { return address == kFrameAddress ? frame.data() : nullptr; }
  std::unique_ptr<Device> OpenDevice(std::string& error) override {
    opens.fetch_add(1);
    if (opens_failing.load() > 0) {
      opens_failing.fetch_sub(1);
      std::this_thread::sleep_for(fail_delay);
      error = "forced failure";
      return nullptr;
    }
    auto fake = std::make_unique<FakeDevice>(events, channels, fail_put, consume);
    device.store(fake.get());
    return fake;
  }
  MixSettings Mix() override { return {}; }
  void Note(std::string_view event, std::string_view) override { events.Add("note-" + std::string(event)); }
  void SleepUntil(Clock::time_point deadline) override { std::this_thread::sleep_until(deadline); }

 private:
  std::thread thread_;
  std::mutex done_mutex_;
  std::condition_variable done_signal_;
  bool done_ = false;
};

template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds limit = 2000ms) {
  const auto end = Clock::now() + limit;
  while (Clock::now() < end) {
    if (predicate()) return true;
    std::this_thread::sleep_for(5ms);
  }
  return predicate();
}

uint32_t Register(FakeHost& host, Output& output) {
  host.output = &output;
  const uint32_t handle = output.Register(kCallback, kArgument);
  host.submit_handle.store(handle);
  return handle;
}

void DeviceOpensAndFramesArrive() {
  FakeHost host;
  Output output(host);
  const uint32_t handle = Register(host, output);
  Check(handle != 0 && (handle & 0xFFFF0000) == kHandleTag, "A nonzero handle with the output's tag");
  Check(WaitFor([&] { return output.Counters().frames_to_device >= 20; }), "Frames reach the device");
  const auto c = output.Counters();
  Check(c.device_opens == 1 && c.open_failures == 0 && c.device.kind == "fake", "One device, opened once");
  Check(c.registers == 1 && c.first_callback == kCallback && c.first_argument == kArgument,
        "The first registration is kept");
  Check(host.last_argument.load() == kArgument, "The callback gets the argument it registered");
  const auto first = host.device.load()->First();
  const float left = 0.25f * StereoFold{}.gain;
  Check(first.size() == kFrameSamples * 2 && std::fabs(first[0] - left) < 1e-6f && first[1] == 0.0f,
        "The frame is folded to stereo");
  Check(c.layout.plausible >= 1, "The frames look like 6 big-endian planes");
  output.Shutdown();
}

// The stage 0 crash: the device does not open. Registering still succeeds, the
// callback keeps its clock on the mute device, and nothing fails in the title.
void DeviceThatDoesNotOpen() {
  FakeHost host;
  host.opens_failing = 1000;
  Output output(host, OutputOptions{.retry = 50ms});
  const uint32_t handle = Register(host, output);
  Check(handle && (handle & 0xFFFF0000) == kHandleTag, "Registering succeeds without a device");
  const auto start = Clock::now();
  Check(WaitFor([&] { return host.calls.load() >= 50; }), "The callback runs on the mute device");
  const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
  const double rate = host.calls.load() / seconds;
  Check(rate > 120 && rate < 260, "About 187.5 callbacks per second on the mute device: " + std::to_string(rate));
  Check(WaitFor([&] { return host.opens.load() >= 3; }), "The device is tried again");
  const auto c = output.Counters();
  Check(c.device.kind == "mute" && c.frames_to_device == 0 && c.frames_muted > 0, "The frames are dropped");
  Check(c.open_failures >= 3 && c.last_open_error == "forced failure", "The failures and their reason are counted");
  output.Shutdown();
}

void DeviceThatOpensLater() {
  FakeHost host;
  host.opens_failing = 2;
  Output output(host, OutputOptions{.retry = 30ms});
  Register(host, output);
  Check(WaitFor([&] { return output.Counters().frames_to_device > 0; }), "A retry opens the device");
  const auto c = output.Counters();
  Check(c.open_failures == 2 && c.device_opens == 1 && c.device.kind == "fake", "Two failures, then the device");
  output.Shutdown();
}

// A driver whose open takes long to fail (a connect timeout): the retries run
// beside the callback, one at a time, and its clock does not wait for them.
void SlowRetry() {
  FakeHost host;
  host.opens_failing = 1000;
  host.fail_delay = 300ms;
  Output output(host, OutputOptions{.retry = 20ms});
  Register(host, output);
  Check(WaitFor([&] { return host.opens.load() >= 2; }), "A retry is under way");
  const uint64_t calls = host.calls.load();
  const int opens = host.opens.load();
  const auto start = Clock::now();
  std::this_thread::sleep_for(1200ms);
  const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
  const double rate = (host.calls.load() - calls) / seconds;
  Check(rate > 120 && rate < 260, "The callback keeps its clock while retries take long: " + std::to_string(rate));
  const int retries = host.opens.load() - opens;
  Check(retries >= 2 && retries <= 6, "The retries go one at a time: " + std::to_string(retries));
  output.Shutdown();  // With a retry in flight: it waits for it, within `join`.
  Check(output.Counters().thread_joined && host.events.Index("note-shutdown") < 0, "The retry finished in time");
}

void DeviceFactsJson() {
  DeviceFacts facts;
  facts.kind = "sdl";
  facts.name = "Fake \"device\"";
  facts.driver = "a\\b\n";
  facts.channels = 2;
  facts.endpoint_channels = 6;
  facts.endpoint_frequency = 48000;
  facts.block_samples = 480;
  const std::string json = DeviceJson(facts);
  Check(json == R"({"kind":"sdl","name":"Fake \"device\"","driver":"a\\b\u000a","channels":2,)"
                R"("endpoint_channels":6,"endpoint_frequency":48000,"block_samples":480})",
        "The device's facts as JSON, its strings escaped: " + json);
}

void DeviceLost() {
  FakeHost host;
  host.fail_put = true;
  Output output(host, OutputOptions{.retry = 60s});
  Register(host, output);
  Check(WaitFor([&] { return output.Counters().device_lost >= 1; }), "A refused frame loses the device");
  Check(WaitFor([&] { return host.calls.load() >= 30; }), "The callback goes on");
  const auto c = output.Counters();
  Check(c.device.kind == "mute" && c.device_lost == 1, "Mute after the loss, lost once until the retry");
  Check(host.events.Index("note-device-lost") >= 0, "The loss is noted");
  output.Shutdown();
}

void DeviceStalled() {
  FakeHost host;
  host.consume = false;  // Takes nothing: the queue fills and stays.
  Output output(host, OutputOptions{.retry = 60s, .stall = 150ms});
  Register(host, output);
  Check(WaitFor([&] { return output.Counters().device_stalls >= 1; }), "A device that takes nothing is dropped");
  const uint64_t calls = host.calls.load();
  Check(WaitFor([&] { return host.calls.load() > calls + 20; }), "The callback goes on after the stall");
  Check(output.Counters().device.kind == "mute", "Mute after the stall");
  output.Shutdown();
}

void BadAddressesAndHandles() {
  FakeHost host;
  Output output(host);
  Check(output.Register(0, kArgument) == 0 && output.Counters().register_refused == 1, "A null callback is refused");
  const uint32_t handle = Register(host, output);
  host.submit_handle.store(0);  // No frames from the callback here.
  output.Submit(handle, 0xDEAD0000);
  output.Submit(kHandleTag | 5, kFrameAddress);
  output.Submit(0x12345678, kFrameAddress);
  output.Unregister(kHandleTag | 7);
  auto c = output.Counters();
  Check(c.frames_invalid == 1, "An unreadable frame address is counted and dropped");
  Check(c.submit_unknown == 2, "Frames of unknown handles are counted and dropped");
  Check(c.unregister_unknown == 1, "Unregistering an unknown handle is counted");
  // Re-registration, as the title's sub_82149870 does: unregister, register.
  output.Unregister(handle);
  const uint32_t again = output.Register(kCallback, kArgument);
  Check(again == handle, "The slot is reused");
  for (uint32_t i = 1; i < kMaximumClients; ++i) output.Register(kCallback, i);
  Check(output.Register(kCallback, 99) == kOverflowHandle, "Past the clients, a handle that is never called");
  output.Submit(kOverflowHandle, kFrameAddress);
  output.Unregister(kOverflowHandle);
  c = output.Counters();
  Check(c.register_overflow == 1 && c.unregisters == 2 && c.registers == 1 + 1 + (kMaximumClients - 1) + 1,
        "Every registration is counted");
  output.Shutdown();
}

void ShutdownInOrder() {
  FakeHost host;
  Output output(host);
  Register(host, output);
  Check(WaitFor([&] { return output.Counters().frames_to_device > 5; }), "Running before the shutdown");
  output.Shutdown();
  const long thread = host.events.Index("thread-exit");
  const long device = host.events.Index("device-closed");
  Check(thread >= 0 && device >= 0, "Thread and device released");
  Check(thread < device, "The thread stops before the device closes");
  const auto c = output.Counters();
  Check(c.shut_down && c.thread_joined, "Shut down with the thread joined");
  const uint64_t frames = c.frames_to_device;
  output.Submit(kHandleTag, kFrameAddress);
  Check(output.Counters().frames_to_device == frames, "After the shutdown no frame reaches a device");
  Check(output.Register(kCallback, kArgument) == kOverflowHandle, "After the shutdown, a handle that is never called");
  output.Shutdown();  // Twice is harmless.
}

void SurroundDevice() {
  FakeHost host;
  host.channels = 6;
  Output output(host);
  Register(host, output);
  Check(WaitFor([&] { return output.Counters().frames_to_device > 0; }), "Frames reach a 5.1 device");
  const auto first = host.device.load()->First();
  Check(first.size() == kFrameSamples * 6 && std::fabs(first[0] - 0.25f * MixSettings{}.surround_gain) < 1e-6f &&
            first[1] == 0.0f, "5.1 keeps the channels, with the headroom gain");
  output.Shutdown();
}
}  // namespace

int main() {
  const std::pair<const char*, void (*)()> cases[] = {
      {"device opens and frames arrive", DeviceOpensAndFramesArrive},
      {"device that does not open", DeviceThatDoesNotOpen},
      {"device that opens later", DeviceThatOpensLater},
      {"a retry that takes long", SlowRetry},
      {"device facts as JSON", DeviceFactsJson},
      {"device lost", DeviceLost},
      {"device stalled", DeviceStalled},
      {"bad addresses and handles", BadAddressesAndHandles},
      {"shutdown in order", ShutdownInOrder},
      {"5.1 device", SurroundDevice},
  };
  for (const auto& [name, test] : cases) {
    try {
      test();
      std::cout << "  ok: " << name << "\n";
    } catch (const std::exception& error) {
      std::cerr << "FAIL in '" << name << "' after " << checks << " checks: " << error.what() << "\n";
      return 1;
    }
  }
  std::cout << "PASS: " << checks << " checks\n";
  return 0;
}
