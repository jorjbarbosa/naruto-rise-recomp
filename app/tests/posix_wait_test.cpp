#include <rex/thread.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include <time.h>

using namespace std::chrono_literals;
using namespace rex::thread;
using Clock = std::chrono::steady_clock;

static void Check(bool passed, const char* message) {
  if (!passed) throw std::runtime_error(message);
}

static double CpuSeconds() {
  timespec value{};
  Check(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0, "thread CPU clock");
  return value.tv_sec + value.tv_nsec / 1e9;
}

struct Handles {
  std::vector<std::unique_ptr<Semaphore>> semaphores;
  std::vector<WaitHandle*> handles;
  explicit Handles(unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
      semaphores.push_back(Semaphore::Create(0, 128));
      handles.push_back(semaphores.back().get());
    }
  }
};

static void Idle(unsigned count, bool alertable, bool all) {
  Handles h(count);
  const auto start = Clock::now();
  const double cpu_start = CpuSeconds();
  for (unsigned i = 0; i < 2; ++i) {
    const auto begin = Clock::now();
    const auto result = all ? WaitAll(h.handles, alertable, 100ms)
                            : WaitAny(h.handles, alertable, 100ms).first;
    Check(result == WaitResult::kTimeout, "idle wait must time out");
    Check(Clock::now() - begin >= 100ms, "timeout must not return early");
  }
  const double cpu = CpuSeconds() - cpu_start;
  const double wall = std::chrono::duration<double>(Clock::now() - start).count();
  std::printf("idle handles=%u alertable=%d all=%d wall=%.6f cpu_percent=%.3f\n",
              count, alertable, all, wall, 100 * cpu / wall);
  Check(cpu / wall < 0.25, "idle wait consumes more than 25% of one CPU");
}

static void Signals(bool alertable) {
  Handles h(9);
  for (unsigned index = 0; index < 9; ++index) {
    std::jthread signal([&, index] {
      std::this_thread::sleep_for(5ms);
      h.semaphores[index]->Release(1, nullptr);
    });
    const auto result = WaitAny(h.handles, alertable, 500ms);
    Check(result.first == WaitResult::kSuccess && result.second == index, "signal index");
    Check(WaitAny(h.handles, false, 0ms).first == WaitResult::kTimeout,
          "semaphore consumed exactly once");
  }
  std::jthread signal([&] {
    std::this_thread::sleep_for(5ms);
    h.semaphores[8]->Release(1, nullptr);
  });
  const auto result = WaitAny(h.handles, alertable);
  Check(result.first == WaitResult::kSuccess && result.second == 8, "infinite wait wakes");
}

static void All(bool alertable) {
  Handles h(2);
  h.semaphores[0]->Release(1, nullptr);
  Check(WaitAll(h.handles, alertable, 5ms) == WaitResult::kTimeout, "partial WaitAll");
  Check(Wait(h.handles[0], false, 0ms) == WaitResult::kSuccess,
        "failed WaitAll must not consume partial signals");
  h.semaphores[0]->Release(1, nullptr);
  std::jthread signal([&] {
    std::this_thread::sleep_for(5ms);
    h.semaphores[1]->Release(1, nullptr);
  });
  Check(WaitAll(h.handles, alertable, 500ms) == WaitResult::kSuccess, "WaitAll wakes");
  Check(WaitAny(h.handles, false, 0ms).first == WaitResult::kTimeout,
        "successful WaitAll consumes all signals");
}

static void Callbacks() {
  Handles h(9);
  auto* self = Thread::GetCurrentThread();
  const auto owner = std::this_thread::get_id();
  std::atomic<unsigned> calls{0};
  std::atomic<bool> correct_thread{false};
  auto callback = [&] {
    correct_thread = std::this_thread::get_id() == owner;
    ++calls;
  };
  self->QueueUserCallback(callback);
  Check(WaitAny(h.handles, false, 5ms).first == WaitResult::kTimeout && calls == 0,
        "non-alertable wait defers callbacks");
  Check(WaitAny(h.handles, true, 500ms).first == WaitResult::kUserCallback && calls == 1,
        "queued callback wakes alertable wait");
  {
    std::jthread queue([&] {
      std::this_thread::sleep_for(5ms);
      self->QueueUserCallback(callback);
    });
    Check(WaitAny(h.handles, true, 500ms).first == WaitResult::kUserCallback,
          "callback interrupts a pending wait");
  }
  Check(calls == 2 && correct_thread, "callbacks run once on the waiting thread");
  Check(WaitAny(h.handles, true, 5ms).first == WaitResult::kTimeout,
        "no callback replay after completion");
}

static void AudioCadence() {
  Handles h(8);
  auto shutdown = Event::CreateManualResetEvent(false);
  h.handles.push_back(shutdown.get());
  std::jthread producer([&] {
    for (unsigned i = 0; i < 128; ++i) {
      h.semaphores[i % 8]->Release(1, nullptr);
      std::this_thread::sleep_for(2ms);
    }
  });
  for (unsigned i = 0; i < 128; ++i) {
    const auto result = WaitAny(h.handles, true, 500ms);
    Check(result.first == WaitResult::kSuccess && result.second < 8,
          "audio-like semaphore cadence");
  }
  producer.join();
  Check(WaitAny(h.handles, false, 0ms).first == WaitResult::kTimeout, "no duplicate signals");
  std::jthread stop([&] {
    std::this_thread::sleep_for(5ms);
    shutdown->Set();
  });
  const auto result = WaitAny(h.handles, true, 500ms);
  Check(result.first == WaitResult::kSuccess && result.second == 8, "shutdown event wakes");
}

int main() {
  try {
    for (unsigned count : {1u, 9u})
      for (bool alertable : {false, true}) Idle(count, alertable, false);
    Idle(9, true, true);
    for (bool alertable : {false, true}) { Signals(alertable); All(alertable); }
    Callbacks();
    AudioCadence();
    std::puts("PASS: idle CPU, timeout, WaitAny/WaitAll, signal consumption, APC and audio-like cadence/shutdown");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
