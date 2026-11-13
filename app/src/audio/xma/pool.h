#pragma once

// The XMA contexts and registers of the native audio (stage 2c/2d).
//
// Pool: 320 contexts of 64 bytes, one array in guest physical memory. The
// title gets a context's virtual address from XMACreateContext; its XAudio
// finds the hardware index as (physical address - register 0x600) / 64
// (sub_82151FB8), and its DARE passes the address to the XMA* imports.
// A Kick decodes at once, on the title's thread that asked, under the
// context's own lock: there is no decoder thread.
//
// Registers (0x7FEA0000): 0x600 holds the array's physical address; 0x606, the
// context the hardware is processing, always reads as none of the title's
// (any value below 0x200: the title compares 0x606 xor 0x200 with its indices,
// sub_82152328), since no decode is ever in progress between two of the
// title's calls. Kick (0x650), Lock (0x690) and Clear (0x6A0) take one bit per
// context, 32 per register. Everything else is counted.
#include "audio/xma/context.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <functional>
#include <string>

namespace narutorise::audio::xma {
constexpr uint32_t kContexts = 320;

class Pool {
 public:
  explicit Pool(GuestMemory& memory);
  // The array: its host address and its guest virtual and physical addresses.
  void Attach(uint8_t* host, uint32_t virtual_address, uint32_t physical_address);
  bool Attached() const { return host_ != nullptr; }
  uint32_t PhysicalBase() const { return physical_; }

  // A free context's virtual address (cleared), or 0 when all are taken.
  uint32_t Allocate();
  void Release(uint32_t index);
  // The index of the context at a virtual address, or -1.
  int Index(uint32_t virtual_address) const;
  uint8_t* Data(uint32_t index) { return host_ + size_t(index) * kContextBytes; }
  std::mutex& Mutex(uint32_t index) { return contexts_[index].Mutex(); }

  void Kick(uint32_t index);
  // Waits for a decode in progress on the context (none ever is, with the
  // decode on the caller's thread, unless two title threads share it).
  void Lock(uint32_t index);
  void Clear(uint32_t index);
  // A new stream on the context (XMAInitializeContext): decoder history dropped.
  void Restart(uint32_t index);

  Counters& Stats() { return counters_; }
  // Called with one line per stream that ends (release, clear, restart).
  void SetTrace(std::function<void(const std::string&)> trace) { trace_ = std::move(trace); }
  uint32_t InUse() const { return in_use_.load(); }
  uint64_t Allocations() const { return allocations_.load(); }
  uint64_t Exhausted() const { return exhausted_.load(); }

 private:
  GuestMemory& memory_;
  uint8_t* host_ = nullptr;
  uint32_t virtual_ = 0, physical_ = 0;
  std::unique_ptr<std::array<Context, kContexts>> contexts_storage_;
  std::array<Context, kContexts>& contexts_;
  std::mutex allocation_mutex_;
  std::array<bool, kContexts> taken_{};
  std::atomic<uint32_t> in_use_{0};
  std::atomic<uint64_t> allocations_{0}, exhausted_{0};
  Counters counters_;
  std::function<void(const std::string&)> trace_;
  std::atomic<uint32_t> traced_{0};
  // Under the context's lock, before it is reset.
  void Trace(uint32_t index, const char* why);
};

class Registers {
 public:
  explicit Registers(Pool& pool) : pool_(pool) {}
  // As the MMIO range calls them: the value is in the guest's byte order.
  uint32_t Read(uint32_t address);
  void Write(uint32_t address, uint32_t value);
  std::string Json() const;

 private:
  Pool& pool_;
  std::atomic<uint64_t> reads_600_{0}, reads_606_{0}, reads_other_{0}, kicks_{0}, locks_{0}, clears_{0},
      writes_601_{0}, writes_other_{0};
};
}  // namespace narutorise::audio::xma
