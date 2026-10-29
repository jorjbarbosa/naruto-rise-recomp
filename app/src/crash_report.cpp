// narutorise - ReXGlue Recompiled Project
//
// Fixed guest fault snapshot and the probe of the object link chain
// (crash_report.h).
//
// Fatal capture belongs to Crashpad outside the unstable process. The SDK
// callback publishes a bounded snapshot of PPCContext without traversing guest
// memory. Reader below is used only by the ordinary transform-cycle probe.

#include "crash_report.h"
#include "diagnostics/incident.h"
#include <array>

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <string>

#include <fmt/format.h>
#include <rex/exception_handler.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>

#include "gpu/common/platform.h"

extern "C" void __imp__sub_8266AE68(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8264C358(PPCContext& __restrict, uint8_t*);

namespace narutorise::crash_report {
namespace {
constexpr uint32_t kChainMax = 64;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  return std::byteswap(*reinterpret_cast<const uint32_t*>(base + address));
}

// Guest reads that cannot fault: the host page must be committed and
// readable. The last readable region is kept, so a walk over nearby addresses
// costs few queries; a Reader lives for one report or one probe.
class Reader {
 public:
  explicit Reader(const uint8_t* base) : base_(base) {}
  bool Get32(uint32_t address, uint32_t* out) {
    if (!Readable(base_ + address, 4)) return false;
    *out = Load32(base_, address);
    return true;
  }

 private:
  bool Readable(const uint8_t* host, size_t size) {
    const uintptr_t begin = reinterpret_cast<uintptr_t>(host), end = begin + size;
    if (begin >= begin_ && end <= end_) return true;
    if (!gpu::platform::ReadableRegion(host, begin_, end_)) {
      begin_ = end_ = 0;
      return false;
    }
    return end <= end_;
  }
  const uint8_t* base_;
  uintptr_t begin_ = 0, end_ = 0;
};

// The SDK has already tried recoverable runtime handlers. Publish only fixed
// guest state, then continue the original exception to Crashpad. No guest
// stack/heap walk, formatter, logger or global report quota runs on this path.
bool Handler(rex::arch::Exception* ex, void*) {
  if (!ex || (ex->code() != rex::arch::Exception::Code::kAccessViolation &&
              ex->code() != rex::arch::Exception::Code::kIllegalInstruction)) return false;
  static thread_local bool observing = false;
  if (observing) return false;
  observing = true;
  const uint64_t address = ex->code() == rex::arch::Exception::Code::kAccessViolation ? ex->fault_address() : 0;
  const uint32_t access = ex->code() == rex::arch::Exception::Code::kAccessViolation
      ? uint32_t(ex->access_violation_operation()) : 0;
  // Preserve the original native header before consulting possibly damaged
  // guest state. A nested fault bypasses this observer and keeps that header.
  narutorise::incident::GuestFault(ex->pc(), address, uint32_t(ex->code()), 0, 0, {}, 0, 0, access);
  auto* state = rex::runtime::ThreadState::Get();
  const auto* ctx = state ? state->context() : nullptr;
  // CTR and r14-r31 are locals of each recompiled function
  // (narutorise_manifest.toml), so their context fields are stale; only r0-r13
  // and LR are published, and the supervisor records the rest as unavailable.
  const std::array<uint64_t, 32> registers = ctx ? std::array<uint64_t, 32>{
      ctx->r0.u64, ctx->r1.u64, ctx->r2.u64, ctx->r3.u64,
      ctx->r4.u64, ctx->r5.u64, ctx->r6.u64, ctx->r7.u64,
      ctx->r8.u64, ctx->r9.u64, ctx->r10.u64, ctx->r11.u64,
      ctx->r12.u64, ctx->r13.u64
  } : std::array<uint64_t, 32>{};
  narutorise::incident::GuestFault(ex->pc(), address,
      uint32_t(ex->code()), state ? state->thread_id() : 0,
      state && state->memory() ? reinterpret_cast<uint64_t>(state->memory()->virtual_membase()) : 0,
      registers, ctx ? ctx->lr : 0, 0,
      access);
  observing = false;
  return false;
}

// The chain a transform update follows (8266AE68): an object not yet activated
// (bit 0 of its flags clear) with the link flag 0x400000 recurses into the
// object at [[object + 28] + 8] + 4 before anything else, with the same test
// there. A cycle in that chain never ends (the stack overflow of the known
// problems). Walks it first, up to kChainMax objects; true on a repeat or a
// chain that long, with the objects and their flags in `text`.
bool LinkCycle(Reader& reader, uint32_t object, std::string* text) {
  uint32_t chain[kChainMax], flags[kChainMax];
  uint32_t n = 0, current = object;
  while (current) {
    uint32_t repeat = n;
    for (uint32_t i = 0; i < n; ++i)
      if (chain[i] == current) repeat = i;
    if (repeat < n || n == kChainMax) {
      for (uint32_t i = 0; i < n; ++i) *text += fmt::format("{}{:08X}({:08X})", i ? " -> " : "", chain[i], flags[i]);
      *text += repeat < n ? fmt::format(" -> {:08X} again", current) : fmt::format(" -> {:08X} ...", current);
      return true;
    }
    uint32_t f = 0, extension = 0, link = 0, next = 0;
    if (!reader.Get32(current, &f)) return false;
    chain[n] = current;
    flags[n++] = f;
    if ((f & 1) || !(f & 0x400000)) return false;
    if (!reader.Get32(current + 28, &extension) || !extension) return false;
    if (!reader.Get32(extension + 8, &link) || !link) return false;
    if (!reader.Get32(link + 4, &next)) return false;
    current = next;
  }
  return false;
}
}  // namespace

void Install() {
  static bool installed = false;
  if (installed) return;
  installed = true;
  rex::arch::ExceptionHandler::Install(Handler, nullptr);  // After the SDK's own.
}
}  // namespace narutorise::crash_report

REX_HOOK_RAW(sub_8266AE68) {
  std::string text;
  narutorise::crash_report::Reader reader(base);
  if (narutorise::crash_report::LinkCycle(reader, ctx.r3.u32, &text)) {
    static std::atomic<uint32_t> cycles{0};
    const uint32_t n = cycles.fetch_add(1) + 1;
    if (n <= 16) REXLOG_ERROR("[narutorise] object link cycle at 8266AE68 (#{}), update skipped: {}", n, text);
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_8266AE68(ctx, base);
}

// The world loads, with the world's id: to place a crash in the return from a fight.
REX_HOOK_RAW(sub_8264C358) {
  REXLOG_INFO("[narutorise] Loading world: 0x{:08X}", ctx.r4.u32);
  __imp__sub_8264C358(ctx, base);
}
