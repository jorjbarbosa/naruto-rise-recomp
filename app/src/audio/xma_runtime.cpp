// The native XMA in the runtime (stages 2c/2d): guest memory for the contexts,
// the audio system that owns the XMA registers, and the XMA* exports behind the
// gate (kernel_xma.cpp). Active with the native backend: the two paths, the
// XMA* imports and the registers, are the native audio's together.
#include "audio/xma_runtime.h"
#include "audio/xma/pool.h"

#include <cstdlib>
#include <memory>
#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/audio.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mmio_handler.h>
#include <rex/system/xmemory.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

namespace narutorise::audio::xma_runtime {
namespace {
using namespace narutorise::audio::xma;
using rex::X_HRESULT;
using rex::X_RESULT;
using rex::X_STATUS;

constexpr uint32_t kPhysicalSize = 0x20000000;  // 512 MB of guest physical memory.
constexpr uint32_t kRegisterBase = 0x7FEA0000;

class RuntimeMemory final : public GuestMemory {
 public:
  uint8_t* Physical(uint32_t address, uint32_t size) override {
    if (!memory_ || address >= kPhysicalSize || size > kPhysicalSize - address) return nullptr;
    return memory_->TranslatePhysical(address);
  }
  // A virtual address's physical one, or UINT32_MAX.
  uint32_t PhysicalOf(uint32_t virtual_address) const {
    return memory_ ? memory_->GetPhysicalAddress(virtual_address) : UINT32_MAX;
  }
  uint8_t* Virtual(uint32_t virtual_address) const { return memory_->TranslateVirtual(virtual_address); }
  void Set(rex::memory::Memory* memory) { memory_ = memory; }

 private:
  rex::memory::Memory* memory_ = nullptr;
};

RuntimeMemory memory;
Pool pool(memory);
Registers registers(pool);
std::atomic<uint64_t> unknown_contexts{0}, bad_buffers{0}, unused_exports{0};

uint32_t ReadThunk(void*, void*, uint32_t address) { return registers.Read(address); }
void WriteThunk(void*, void*, uint32_t address, uint32_t value) { registers.Write(address, value); }

class NativeAudioSystem final : public rex::system::IAudioSystem {
 public:
  X_STATUS Setup(rex::system::KernelState* kernel) override {
    auto* guest = kernel->memory();
    memory.Set(guest);
    const uint32_t array = guest->SystemHeapAlloc(kContexts * kContextBytes, 256, rex::memory::kSystemHeapPhysical);
    if (!array) {
      REXLOG_ERROR("[narutorise] Audio XMA: no guest memory for the {} contexts", kContexts);
      return X_STATUS_NO_MEMORY;
    }
    pool.Attach(guest->TranslateVirtual(array), array, guest->GetPhysicalAddress(array));
    if (const char* dump = std::getenv("NARUTORISE_AUDIO_XMA_DUMP"); dump && *dump) {
      const char* loops = std::getenv("NARUTORISE_AUDIO_DUMP_LOOPS");
      SetDump(dump, 40, loops && *loops && *loops != '0');
    }
    if (const char* trace = std::getenv("NARUTORISE_AUDIO_XMA_TRACE"); trace && *trace && *trace != '0') {
      SetStreamTrace(true);
      pool.SetTrace([](const std::string& line) { REXLOG_INFO("[narutorise] Audio XMA stream: {}", line); });
    }
    guest->AddVirtualMappedRange(kRegisterBase, 0xFFFF0000, 0x0000FFFF, nullptr, ReadThunk, WriteThunk);
    REXLOG_INFO("[narutorise] Audio XMA: native; {} contexts at 0x{:08X} (physical 0x{:08X}), registers at 0x{:08X}",
                kContexts, array, pool.PhysicalBase(), kRegisterBase);
    return X_STATUS_SUCCESS;
  }
  void Shutdown() override {}
};

// The context a title address names, under its lock; null when it is none.
struct Locked {
  int index = -1;
  std::unique_lock<std::mutex> lock;
  uint8_t* data = nullptr;
};
Locked Find(uint32_t virtual_address) {
  Locked found;
  found.index = pool.Index(virtual_address);
  if (found.index < 0) {
    unknown_contexts.fetch_add(1, std::memory_order_relaxed);
    return found;
  }
  found.lock = std::unique_lock(pool.Mutex(found.index));
  found.data = pool.Data(found.index);
  return found;
}
template <typename F>
uint32_t Edit(uint32_t context, F change) {
  Locked c = Find(context);
  if (!c.data) return X_E_INVALIDARG;
  ContextData d = ContextData::Load(c.data);
  change(d);
  d.Store(c.data);
  return X_ERROR_SUCCESS;
}
template <typename F>
uint32_t Get(uint32_t context, F field) {
  Locked c = Find(context);
  if (!c.data) return 0;
  return field(ContextData::Load(c.data));
}
uint32_t Big(const uint8_t* p, uint32_t offset) {
  return (uint32_t(p[offset]) << 24) | (uint32_t(p[offset + 1]) << 16) | (uint32_t(p[offset + 2]) << 8) | p[offset + 3];
}
// A buffer's physical address; 0 stays 0 (no buffer), a bad one is counted.
uint32_t Physical(uint32_t virtual_address) {
  if (!virtual_address) return 0;
  const uint32_t physical = memory.PhysicalOf(virtual_address);
  if (physical == UINT32_MAX) {
    bad_buffers.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  return physical;
}

// The exports. Arguments as the title passes them.
u32 CreateContext(mapped_u32 out) {
  if (!out) return X_E_INVALIDARG;
  const uint32_t context = pool.Allocate();
  *out = context;
  return context ? X_STATUS_SUCCESS : X_STATUS_NO_MEMORY;
}
u32 ReleaseContext(mapped_void context) {
  const int index = pool.Index(context.guest_address());
  if (index < 0) {
    unknown_contexts.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  pool.Release(index);
  return 0;
}
// XMA_CONTEXT_INIT: input buffer 0 and its packets, buffer 1 and its packets,
// read offset, output buffer and its blocks, work buffer, subframes to decode,
// channels - 1, rate index, loop start, loop end, loop count, subframe end,
// subframe skip (big-endian words, then 3 bytes).
u32 InitializeContext(mapped_void context, mapped_void init_ptr) {
  const auto* init = static_cast<const uint8_t*>(init_ptr.host_address());
  if (!init) return X_E_INVALIDARG;
  const int index = pool.Index(context.guest_address());
  if (index >= 0) pool.Restart(index);
  return Edit(context.guest_address(), [&](ContextData& d) {
    d = ContextData{};
    d.input_buffer_0_ptr = Physical(Big(init, 0));
    d.input_buffer_0_packet_count = Big(init, 4);
    d.input_buffer_1_ptr = Physical(Big(init, 8));
    d.input_buffer_1_packet_count = Big(init, 12);
    d.input_buffer_read_offset = Big(init, 16);
    d.output_buffer_ptr = Physical(Big(init, 20));
    d.output_buffer_block_count = Big(init, 24);
    d.work_buffer_ptr = Physical(Big(init, 28));
    d.subframe_decode_count = Big(init, 32);
    d.is_stereo = Big(init, 36) >= 1;
    d.sample_rate = Big(init, 40);
    d.loop_start = Big(init, 44);
    d.loop_end = Big(init, 48);
    d.loop_count = init[52];
    d.loop_subframe_end = init[53];
    d.loop_subframe_skip = init[54];
  });
}
// XMA_LOOP_DATA: loop start, loop end, then count, subframe end, subframe skip.
u32 SetLoopData(mapped_void context, mapped_void loop_ptr) {
  const auto* loop = static_cast<const uint8_t*>(loop_ptr.host_address());
  if (!loop) return X_E_INVALIDARG;
  return Edit(context.guest_address(), [&](ContextData& d) {
    d.loop_start = Big(loop, 0);
    d.loop_end = Big(loop, 4);
    d.loop_count = loop[8];
    d.loop_subframe_end = loop[9];
    d.loop_subframe_skip = loop[10];
  });
}
u32 GetInputBufferReadOffset(mapped_void c) {
  return Get(c.guest_address(), [](const ContextData& d) { return uint32_t(d.input_buffer_read_offset); });
}
u32 SetInputBufferReadOffset(mapped_void c, u32 value) {
  return Edit(c.guest_address(), [&](ContextData& d) { d.input_buffer_read_offset = value; });
}
u32 SetInputBuffer0(mapped_void c, mapped_void buffer, u32 packets) {
  return Edit(c.guest_address(), [&](ContextData& d) {
    d.input_buffer_0_ptr = Physical(buffer.guest_address());
    d.input_buffer_0_packet_count = packets;
  });
}
u32 SetInputBuffer1(mapped_void c, mapped_void buffer, u32 packets) {
  return Edit(c.guest_address(), [&](ContextData& d) {
    d.input_buffer_1_ptr = Physical(buffer.guest_address());
    d.input_buffer_1_packet_count = packets;
  });
}
u32 IsInputBuffer0Valid(mapped_void c) {
  return Get(c.guest_address(), [](const ContextData& d) { return uint32_t(d.input_buffer_0_valid); });
}
u32 IsInputBuffer1Valid(mapped_void c) {
  return Get(c.guest_address(), [](const ContextData& d) { return uint32_t(d.input_buffer_1_valid); });
}
u32 SetInputBuffer0Valid(mapped_void c) {
  return Edit(c.guest_address(), [](ContextData& d) { d.input_buffer_0_valid = 1; });
}
u32 SetInputBuffer1Valid(mapped_void c) {
  return Edit(c.guest_address(), [](ContextData& d) { d.input_buffer_1_valid = 1; });
}
u32 IsOutputBufferValid(mapped_void c) {
  return Get(c.guest_address(), [](const ContextData& d) { return uint32_t(d.output_buffer_valid); });
}
u32 SetOutputBufferValid(mapped_void c) {
  return Edit(c.guest_address(), [](ContextData& d) { d.output_buffer_valid = 1; });
}
u32 GetOutputBufferReadOffset(mapped_void c) {
  return Get(c.guest_address(), [](const ContextData& d) { return uint32_t(d.output_buffer_read_offset); });
}
u32 SetOutputBufferReadOffset(mapped_void c, u32 value) {
  return Edit(c.guest_address(), [&](ContextData& d) { d.output_buffer_read_offset = value; });
}
u32 GetOutputBufferWriteOffset(mapped_void c) {
  return Get(c.guest_address(), [](const ContextData& d) { return uint32_t(d.output_buffer_write_offset); });
}
u32 GetPacketMetadata(mapped_void c) {
  unused_exports.fetch_add(1, std::memory_order_relaxed);
  return Get(c.guest_address(), [](const ContextData& d) { return uint32_t(d.packet_metadata); });
}
u32 EnableContext(mapped_void c) {
  const int index = pool.Index(c.guest_address());
  if (index < 0) {
    unknown_contexts.fetch_add(1, std::memory_order_relaxed);
    return X_E_INVALIDARG;
  }
  pool.Kick(index);
  return 0;
}
u32 DisableContext(mapped_void c, u32 wait) {
  (void)wait;
  const int index = pool.Index(c.guest_address());
  if (index < 0) {
    unknown_contexts.fetch_add(1, std::memory_order_relaxed);
    return X_E_INVALIDARG;
  }
  pool.Lock(index);
  return 0;
}
u32 BlockWhileInUse(mapped_void c) {
  unused_exports.fetch_add(1, std::memory_order_relaxed);
  const int index = pool.Index(c.guest_address());
  if (index >= 0) pool.Lock(index);
  return 0;
}
}  // namespace

std::unique_ptr<rex::system::IAudioSystem> CreateSystem() { return std::make_unique<NativeAudioSystem>(); }

std::string Json() {
  Counters& c = pool.Stats();
  const uint64_t kicks = c.kicks.load();
  return fmt::format(
      "{{\"contexts_in_use\":{},\"allocations\":{},\"exhausted\":{},\"kicks\":{},\"kicks_without_output\":{},"
      "\"frames\":{},\"blocks\":{},\"decode_failures\":{},\"bad_data\":{},\"bad_addresses\":{},\"stalls\":{},"
      "\"buffers_consumed\":{},\"streams_ended\":{},\"loops\":{},\"skipped_packets\":{},\"outside_shape\":{},"
      "\"kick_us_mean\":{:.1f},\"kick_us_max\":{:.1f},\"kicks_over_1ms\":{},\"kicks_over_budget\":{},"
      "\"unknown_contexts\":{},\"bad_buffers\":{},"
      "\"unused_exports\":{},\"registers\":{}}}",
      pool.InUse(), pool.Allocations(), pool.Exhausted(), kicks, c.kicks_without_output.load(), c.frames.load(),
      c.blocks.load(), c.decode_failures.load(), c.bad_data.load(), c.bad_addresses.load(), c.stalls.load(),
      c.buffers_consumed.load(), c.streams_ended.load(), c.loops.load(), c.skipped_packets.load(),
      c.outside_shape.load(), kicks ? c.kick_ns_total.load() / 1000.0 / kicks : 0.0, c.kick_ns_max.load() / 1000.0,
      c.kicks_over_1ms.load(), c.kicks_over_budget.load(),
      unknown_contexts.load(), bad_buffers.load(), unused_exports.load(), registers.Json());
}

// The exports by the gate's slot (kernel_xma.cpp).
void Call(int slot, PPCContext& ctx, uint8_t* base) {
  using rex::ppc::HostToGuestFunction;
  switch (slot) {
    case 0: HostToGuestFunction<CreateContext>(ctx, base); break;
    case 1: HostToGuestFunction<ReleaseContext>(ctx, base); break;
    case 2: HostToGuestFunction<InitializeContext>(ctx, base); break;
    case 3: HostToGuestFunction<SetLoopData>(ctx, base); break;
    case 4: HostToGuestFunction<GetInputBufferReadOffset>(ctx, base); break;
    case 5: HostToGuestFunction<SetInputBufferReadOffset>(ctx, base); break;
    case 6: HostToGuestFunction<SetInputBuffer0>(ctx, base); break;
    case 7: HostToGuestFunction<IsInputBuffer0Valid>(ctx, base); break;
    case 8: HostToGuestFunction<SetInputBuffer0Valid>(ctx, base); break;
    case 9: HostToGuestFunction<SetInputBuffer1>(ctx, base); break;
    case 10: HostToGuestFunction<IsInputBuffer1Valid>(ctx, base); break;
    case 11: HostToGuestFunction<SetInputBuffer1Valid>(ctx, base); break;
    case 12: HostToGuestFunction<IsOutputBufferValid>(ctx, base); break;
    case 13: HostToGuestFunction<SetOutputBufferValid>(ctx, base); break;
    case 14: HostToGuestFunction<GetOutputBufferReadOffset>(ctx, base); break;
    case 15: HostToGuestFunction<SetOutputBufferReadOffset>(ctx, base); break;
    case 16: HostToGuestFunction<GetOutputBufferWriteOffset>(ctx, base); break;
    case 17: HostToGuestFunction<GetPacketMetadata>(ctx, base); break;
    case 18: HostToGuestFunction<EnableContext>(ctx, base); break;
    case 19: HostToGuestFunction<DisableContext>(ctx, base); break;
    case 20: HostToGuestFunction<BlockWhileInUse>(ctx, base); break;
    default: break;
  }
}
}  // namespace narutorise::audio::xma_runtime
