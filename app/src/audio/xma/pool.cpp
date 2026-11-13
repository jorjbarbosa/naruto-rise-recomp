// The XMA contexts and registers of the native audio (pool.h).
#include "audio/xma/pool.h"

#include <bit>
#include <cstring>
#include <fmt/format.h>

namespace narutorise::audio::xma {
Pool::Pool(GuestMemory& memory)
    : memory_(memory),
      contexts_storage_(std::make_unique<std::array<Context, kContexts>>()),
      contexts_(*contexts_storage_) {}

void Pool::Attach(uint8_t* host, uint32_t virtual_address, uint32_t physical_address) {
  host_ = host;
  virtual_ = virtual_address;
  physical_ = physical_address;
  std::memset(host_, 0, size_t(kContexts) * kContextBytes);
}

int Pool::Index(uint32_t virtual_address) const {
  if (!host_ || virtual_address < virtual_ || (virtual_address - virtual_) % kContextBytes) return -1;
  const uint32_t index = (virtual_address - virtual_) / kContextBytes;
  return index < kContexts ? static_cast<int>(index) : -1;
}

uint32_t Pool::Allocate() {
  if (!host_) return 0;
  std::lock_guard lock(allocation_mutex_);
  for (uint32_t i = 0; i < kContexts; ++i) {
    if (taken_[i]) continue;
    taken_[i] = true;
    {
      std::lock_guard context_lock(contexts_[i].Mutex());
      contexts_[i].Reset();
      std::memset(Data(i), 0, kContextBytes);
    }
    in_use_.fetch_add(1);
    allocations_.fetch_add(1);
    return virtual_ + i * kContextBytes;
  }
  exhausted_.fetch_add(1);
  return 0;
}

void Pool::Trace(uint32_t index, const char* why) {
  const StreamStats& s = contexts_[index].Stream();
  if (!trace_ || s.kicks == 0 || traced_.fetch_add(1) >= 2000) return;
  const ContextData d = ContextData::Load(Data(index));
  const uint32_t ring = d.output_buffer_block_count ? d.output_buffer_block_count : 1;
  const uint32_t unread = (d.output_buffer_write_offset + ring - d.output_buffer_read_offset % ring) % ring;
  trace_(fmt::format("{} ctx {} rate {} ch {} ring {} packets {} loop {} loops {} kicks {} frames {}/{} blocks {} "
                     "waiting {} unread {} stalls {} consumed {} in_valid {}/{}",
                     why, index, s.rate, s.channels, s.ring, s.packets, s.loop, s.loops, s.kicks, s.frames,
                     s.frames_in_input,
                     s.blocks, contexts_[index].Pending(), unread, s.stalls, s.consumed, uint32_t(d.input_buffer_0_valid),
                     uint32_t(d.input_buffer_1_valid)));
}

void Pool::Release(uint32_t index) {
  {
    std::lock_guard context_lock(contexts_[index].Mutex());
    Trace(index, "release");
    contexts_[index].Reset();
    std::memset(Data(index), 0, kContextBytes);
  }
  std::lock_guard lock(allocation_mutex_);
  if (taken_[index]) {
    taken_[index] = false;
    in_use_.fetch_sub(1);
  }
}

void Pool::Kick(uint32_t index) {
  std::lock_guard lock(contexts_[index].Mutex());
  contexts_[index].Kick(Data(index), memory_, counters_);
}

void Pool::Lock(uint32_t index) { std::lock_guard lock(contexts_[index].Mutex()); }

void Pool::Clear(uint32_t index) {
  std::lock_guard lock(contexts_[index].Mutex());
  Trace(index, "clear");
  contexts_[index].Reset();
  ContextData d = ContextData::Load(Data(index));
  d.input_buffer_0_valid = d.input_buffer_1_valid = 0;
  d.output_buffer_valid = 0;
  d.current_buffer = 0;
  d.input_buffer_read_offset = kPacketHeaderBits;
  d.output_buffer_read_offset = d.output_buffer_write_offset = 0;
  d.Store(Data(index));
}

void Pool::Restart(uint32_t index) {
  std::lock_guard lock(contexts_[index].Mutex());
  Trace(index, "restart");
  contexts_[index].Reset();
}

uint32_t Registers::Read(uint32_t address) {
  const uint32_t r = (address & 0xFFFF) >> 2;
  if (r == 0x600) {
    reads_600_.fetch_add(1, std::memory_order_relaxed);
    return std::byteswap(pool_.PhysicalBase());
  }
  if (r == 0x606) {
    reads_606_.fetch_add(1, std::memory_order_relaxed);
    return 0;  // None of the title's contexts is being processed.
  }
  reads_other_.fetch_add(1, std::memory_order_relaxed);
  return 0;
}

void Registers::Write(uint32_t address, uint32_t value) {
  const uint32_t r = (address & 0xFFFF) >> 2;
  const uint32_t bits = std::byteswap(value);
  const auto each = [&](uint32_t first, auto action) {
    const uint32_t base = (r - first) * 32;
    for (uint32_t bit = 0; bit < 32; ++bit)
      if ((bits >> bit) & 1 && base + bit < kContexts) action(base + bit);
  };
  if (r >= 0x650 && r <= 0x659) {
    kicks_.fetch_add(1, std::memory_order_relaxed);
    each(0x650, [&](uint32_t i) { pool_.Kick(i); });
  } else if (r >= 0x690 && r <= 0x699) {
    locks_.fetch_add(1, std::memory_order_relaxed);
    each(0x690, [&](uint32_t i) { pool_.Lock(i); });
  } else if (r >= 0x6A0 && r <= 0x6A9) {
    clears_.fetch_add(1, std::memory_order_relaxed);
    each(0x6A0, [&](uint32_t i) { pool_.Clear(i); });
  } else if (r == 0x601) {
    writes_601_.fetch_add(1, std::memory_order_relaxed);
  } else {
    writes_other_.fetch_add(1, std::memory_order_relaxed);
  }
}

std::string Registers::Json() const {
  return fmt::format(
      "{{\"reads_600\":{},\"reads_606\":{},\"reads_other\":{},\"kick_writes\":{},\"lock_writes\":{},"
      "\"clear_writes\":{},\"writes_601\":{},\"writes_other\":{}}}",
      reads_600_.load(), reads_606_.load(), reads_other_.load(), kicks_.load(), locks_.load(), clears_.load(),
      writes_601_.load(), writes_other_.load());
}
}  // namespace narutorise::audio::xma
