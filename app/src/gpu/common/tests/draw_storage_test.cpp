#include "gpu/common/constant_delta.h"
#include "gpu/backend/dispatcher.h"
#include <array>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <random>
#include <thread>

using namespace gpu;
struct Packet final : StageTask, DrawStorageAllocated<Packet> {
  uint32_t id = 0;
  ConstantBytes bytes;
  void Run() override {}
};

int main() {
  bool passed = true;
  std::array<uint8_t, 4096> source;
  for (size_t i = 0; i < source.size(); ++i) source[i] = uint8_t(i * 7 + i / 64);
  std::mt19937_64 random(0xC0FFEE);
  for (unsigned trial = 0; trial < 2048; ++trial) {
    const uint64_t mask = trial == 0 ? 0 : trial == 1 ? ~0ull : trial == 2 ? 0xAAAAAAAAAAAAAAAAull : random();
    ConstantBytes delta;
    PackConstantDelta(source, mask, delta);
    std::array<uint8_t, 4096> decoded;
    decoded.fill(0xCD);
    size_t at = 0;
    while (at < delta.size()) {
      uint16_t first = 0, count = 0;
      if (at + 4 > delta.size()) { passed = false; break; }
      std::memcpy(&first, delta.data() + at, 2);
      std::memcpy(&count, delta.data() + at + 2, 2);
      if (!count || first + count > 256 || at + 4 + size_t(count) * 16 > delta.size()) { passed = false; break; }
      std::memcpy(decoded.data() + size_t(first) * 16, delta.data() + at + 4, size_t(count) * 16);
      at += 4 + size_t(count) * 16;
    }
    for (size_t i = 0; i < decoded.size(); ++i)
      passed &= decoded[i] == ((mask >> (63 - i / 64)) & 1 ? source[i] : uint8_t(0xCD));
  }
  const bool deltas_ok = passed;
  // Capture -> translator -> owner, with actual DrawRequest moves. Deleting
  // the polymorphic task must leave the moved bytes valid until the owner
  // consumes them. Bounded rounds force freed storage to circulate again.
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<std::unique_ptr<StageTask>> captures;
  std::deque<DrawRequest> requests;
  bool producing = true, translating = true;
  uint32_t consumed = 0;
  std::atomic<bool> payloads_ok{true};
  std::thread translator([&] {
    for (;;) {
      std::unique_ptr<StageTask> task;
      {
        std::unique_lock lock(mutex);
        wake.wait(lock, [&] { return !captures.empty() || !producing; });
        if (captures.empty()) break;
        task = std::move(captures.front()); captures.pop_front();
      }
      auto& packet = static_cast<Packet&>(*task);
      auto* original = packet.bytes.data();
      DrawRequest request;
      request.index_count = packet.id;
      request.vs_constants = std::move(packet.bytes);
      if (request.vs_constants.data() != original) payloads_ok = false;
      task.reset();  // Object recycled before the owner's read of its bytes.
      {
        std::lock_guard lock(mutex);
        requests.push_back(std::move(request));
      }
      wake.notify_all();
    }
    { std::lock_guard lock(mutex); translating = false; }
    wake.notify_all();
  });
  std::thread owner([&] {
    uint32_t expected = 0;
    for (;;) {
      DrawRequest request;
      {
        std::unique_lock lock(mutex);
        wake.wait(lock, [&] { return !requests.empty() || !translating; });
        if (requests.empty()) break;
        request = std::move(requests.front()); requests.pop_front();
      }
      if (request.index_count != expected || request.vs_constants.size() != 1024) payloads_ok = false;
      for (auto byte : request.vs_constants) if (byte != uint8_t(expected)) payloads_ok = false;
      ++expected;
      request = {};  // Release the owner's storage before admitting the next round.
      { std::lock_guard lock(mutex); consumed = expected; }
      wake.notify_all();
    }
  });
  uint64_t warm_allocations = 0;
  for (uint32_t round = 0; round < 100; ++round) {
    for (uint32_t i = 0; i < 64; ++i) {
      auto packet = std::make_unique<Packet>();
      packet->id = round * 64 + i;
      packet->bytes.assign(1024, uint8_t(packet->id));
      { std::lock_guard lock(mutex); captures.push_back(std::move(packet)); }
      wake.notify_all();
    }
    std::unique_lock lock(mutex);
    wake.wait(lock, [&] { return consumed == (round + 1) * 64; });
    if (round == 9) warm_allocations = GetDrawStorageStats().upstream_allocations;
  }
  const auto final_allocations = GetDrawStorageStats().upstream_allocations;
  { std::lock_guard lock(mutex); producing = false; }
  wake.notify_all(); translator.join(); owner.join();
  passed &= payloads_ok && consumed == 6400 && final_allocations - warm_allocations < 64;
  std::cout << "{\"passed\":" << (passed ? "true" : "false") << ",\"packets\":" << consumed
            << ",\"deltas_ok\":" << (deltas_ok ? "true" : "false")
            << ",\"payloads_ok\":" << (payloads_ok ? "true" : "false")
            << ",\"upstream_allocations_after_warmup\":" << final_allocations - warm_allocations << "}\n";
  return passed ? 0 : 1;
}
