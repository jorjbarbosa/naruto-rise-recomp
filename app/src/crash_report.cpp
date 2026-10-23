// narutorise - ReXGlue Recompiled Project
//
// Crash report of a guest fault and the probe of the object link chain
// (crash_report.h).
//
// The guest stack: a function saves its return address at -8 of the caller's
// r1 and links the frames with the back chain at [r1]. From the r1 of the
// faulting thread, the caller's r1 is [r1] and the return address that frame
// saved is [[r1] - 8]; the walk stays inside the thread's stack (read from its
// PCR) and stops at the thread's entry or at anything that is not a frame
// above the last one. Equal addresses in a row are counted (a recursion shows
// as one address times its depth).
//
// Reads of guest memory check the host page first (platform::ReadableRegion).
// A __try does not guard a plain load in this build (clang without
// -fasync-exceptions covers only the calls inside it): on 2026-10-18 the walk
// read the guard page above a stack and the process ended inside the handler,
// before the return addresses.

#include "crash_report.h"

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
constexpr uint64_t kGuestArena = 0x100000000ull;  // The virtual base and the 4 GB after it.
constexpr uint32_t kMaxFrames = 65536;
constexpr uint32_t kMaxRuns = 96;
constexpr uint32_t kMaxReports = 4;
constexpr uint32_t kChainMax = 64;
// The dlmalloc state of the game's main heap (the one of the known heap
// corruption): smallmap +0, treemap +4, dvsize +8, topsize +12, least_addr +16,
// dv +20, top +24, the 32 small bins from +36 (8 bytes each: the head is a
// pseudo chunk whose fd is at +8 and bk at +12), the segment list from +444
// (base, size, next, flags).
constexpr uint32_t kHeapState = 0x82701E04;
constexpr uint32_t kSmallBins = 32;
constexpr uint32_t kMaxSegments = 32;

std::atomic<uint32_t> reports{0};
thread_local bool reporting = false;

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

struct Run {
  uint32_t address, count;
};

// The thread's stack from its PCR (r13): +0x70 the base (high), +0x74 the end
// (low); zeros when unreadable.
void StackRange(Reader& reader, const PPCContext& ctx, uint32_t* end, uint32_t* base) {
  if (!reader.Get32(ctx.r13.u32 + 0x70, base)) *base = 0;
  if (!reader.Get32(ctx.r13.u32 + 0x74, end)) *end = 0;
}

// The return addresses of a guest thread, from its lr and then the ones its
// frames saved, inside [stack_end, stack_base) when that range is known.
std::string ReturnChain(Reader& reader, uint32_t lr, uint32_t sp, uint32_t stack_end, uint32_t stack_base,
                        uint32_t* frames_out) {
  Run runs[kMaxRuns]{};
  uint32_t run_count = 0, frames = 0, beyond = 0;
  auto add = [&](uint32_t address) {
    ++frames;
    if (run_count && runs[run_count - 1].address == address) {
      ++runs[run_count - 1].count;
      return;
    }
    if (run_count < kMaxRuns) runs[run_count++] = {address, 1};
    else ++beyond;
  };
  add(lr);
  const bool bounded = stack_base > stack_end;
  for (uint32_t i = 0; i < kMaxFrames; ++i) {
    if (bounded && (sp < stack_end || sp >= stack_base)) break;
    uint32_t next = 0, address = 0;
    if (!reader.Get32(sp, &next) || next <= sp || next - sp > 0x100000 || (next & 7)) break;
    if (bounded && next > stack_base) break;
    if (!reader.Get32(next - 8, &address)) break;
    add(address);
    sp = next;
  }
  std::string text;
  for (uint32_t i = 0; i < run_count; ++i) {
    if (!text.empty()) text += ' ';
    text += runs[i].count > 1 ? fmt::format("{:08X}x{}", runs[i].address, runs[i].count)
                              : fmt::format("{:08X}", runs[i].address);
  }
  if (beyond) text += fmt::format(" ...+{}", beyond);
  *frames_out = frames;
  return text;
}

// The game's heap at the fault: the dlmalloc state, its segments and the
// small bins that hold something, with the fault placed among them. The
// allocator only checks a bin pointer against least_addr; a failed check
// traps (type 22), keeps the chunk in the bin and hands it out again, and a
// later pointer above least_addr is written through (fd at +8, bk at +12).
// So a fault at +12 of a garbage pointer names the bin whose head holds it.
void HeapReport(Reader& reader, uint32_t fault) {
  uint32_t smallmap = 0, treemap = 0, dvsize = 0, topsize = 0, least = 0, dv = 0, top = 0;
  if (!reader.Get32(kHeapState, &smallmap) || !reader.Get32(kHeapState + 4, &treemap) ||
      !reader.Get32(kHeapState + 8, &dvsize) || !reader.Get32(kHeapState + 12, &topsize) ||
      !reader.Get32(kHeapState + 16, &least) || !reader.Get32(kHeapState + 20, &dv) ||
      !reader.Get32(kHeapState + 24, &top)) {
    REXLOG_ERROR("[narutorise] crash report: heap state 0x{:08X} unreadable", kHeapState);
    return;
  }
  std::string segments;
  bool inside = false;
  uint32_t segment = kHeapState + 444;
  for (uint32_t i = 0; i < kMaxSegments && segment; ++i) {
    uint32_t sbase = 0, size = 0, next = 0, flags = 0;
    if (!reader.Get32(segment, &sbase) || !reader.Get32(segment + 4, &size) || !reader.Get32(segment + 8, &next) ||
        !reader.Get32(segment + 12, &flags))
      break;
    if (!sbase && !size) break;
    const bool holds = fault >= sbase && fault < sbase + size;
    inside = inside || holds;
    segments += fmt::format("{}{:08X}..{:08X}/{:X}{}", i ? " " : "", sbase, sbase + size, flags, holds ? "<-fault" : "");
    segment = next;
  }
  REXLOG_ERROR(
      "[narutorise] crash report: heap 0x{:08X}: smallmap 0x{:08X} treemap 0x{:08X} least_addr 0x{:08X} dv 0x{:08X} "
      "({}) top 0x{:08X} ({}); fault {} the segments: {}",
      kHeapState, smallmap, treemap, least, dv, dvsize, top, topsize, inside ? "inside" : "outside", segments);
  std::string bins;
  for (uint32_t index = 0; index < kSmallBins; ++index) {
    const uint32_t head = kHeapState + 36 + index * 8;
    uint32_t fd = 0, bk = 0;
    if (!reader.Get32(head + 8, &fd) || !reader.Get32(head + 12, &bk)) break;
    if (fd == head && bk == head) continue;
    const bool names = fd == fault - 12 || fd == fault - 8 || bk == fault - 12 || bk == fault - 8;
    bins += fmt::format("{}{}:{:08X}/{:08X}{}", bins.empty() ? "" : " ", index * 8, fd, bk, names ? "<-fault" : "");
  }
  REXLOG_ERROR("[narutorise] crash report: small bins (size:fd/bk): {}", bins.empty() ? "none" : bins);
  for (uint32_t offset : {8u, 12u}) {
    const uint32_t chunk = fault - offset;
    uint32_t foot = 0, head = 0, fd = 0, bk = 0;
    if (reader.Get32(chunk, &foot) && reader.Get32(chunk + 4, &head) && reader.Get32(chunk + 8, &fd) &&
        reader.Get32(chunk + 12, &bk))
      REXLOG_ERROR("[narutorise] crash report: chunk at fault-{} 0x{:08X}: prev_foot 0x{:08X} head 0x{:08X} fd 0x{:08X} "
                   "bk 0x{:08X}",
                   offset, chunk, foot, head, fd, bk);
    else
      REXLOG_ERROR("[narutorise] crash report: chunk at fault-{} 0x{:08X}: no memory", offset, chunk);
  }
}

void Report(const char* kind, uint32_t code, uint64_t host_fault) {
  auto* state = rex::runtime::ThreadState::Get();
  if (!state || !state->context() || !state->memory()) {
    REXLOG_ERROR("[narutorise] crash report: {} (code 0x{:08X}) on a host thread without a guest context", kind,
                 code);
    return;
  }
  PPCContext* ctx = state->context();
  const uint8_t* base = state->memory()->virtual_membase();
  const uint64_t host_base = reinterpret_cast<uint64_t>(base);
  const bool guest = host_fault >= host_base && host_fault < host_base + kGuestArena;
  const std::string fault = guest ? fmt::format("guest 0x{:08X}", uint32_t(host_fault - host_base))
                                  : fmt::format("host 0x{:016X}", host_fault);
  Reader reader(base);
  uint32_t stack_end = 0, stack_base = 0;
  StackRange(reader, *ctx, &stack_end, &stack_base);
  REXLOG_ERROR(
      "[narutorise] crash report: {} of {} (code 0x{:08X}) on guest thread 0x{:X}: r1 0x{:08X}, lr 0x{:08X}, "
      "r3 0x{:08X}, r4 0x{:08X}, r12 0x{:08X}, stack 0x{:08X}..0x{:08X}",
      kind, fault, code, state->thread_id(), ctx->r1.u32, uint32_t(ctx->lr), ctx->r3.u32, ctx->r4.u32, ctx->r12.u32,
      stack_end, stack_base);
  rex::FlushLogging();
  uint32_t frames = 0;
  const std::string chain = ReturnChain(reader, uint32_t(ctx->lr), ctx->r1.u32, stack_end, stack_base, &frames);
  REXLOG_ERROR("[narutorise] crash report: {} return addresses from the top (lr, then the saved ones): {}", frames,
               chain);
  rex::FlushLogging();
  if (guest) HeapReport(reader, uint32_t(host_fault - host_base));
  rex::FlushLogging();
}

// In the SDK's handler chain (rex::arch::ExceptionHandler: a vectored handler on
// Windows, the signal handlers elsewhere), after its own handlers, which
// resume the faults they handle (the write watch, the MMIO windows); the
// report sees only the rest and never handles the fault. A fault of the
// report itself passes. The chain gives access violations and illegal
// instructions; the report keeps their Windows codes on every system.
bool Handler(rex::arch::Exception* ex, void*) {
  if (!ex) return false;
  const char* kind = nullptr;
  uint32_t code = 0;
  switch (ex->code()) {
    case rex::arch::Exception::Code::kAccessViolation:
      switch (ex->access_violation_operation()) {
        case rex::arch::Exception::AccessViolationOperation::kRead: kind = "read"; break;
        case rex::arch::Exception::AccessViolationOperation::kWrite: kind = "write"; break;
        default: kind = "access"; break;
      }
      code = 0xC0000005;  // EXCEPTION_ACCESS_VIOLATION
      break;
    case rex::arch::Exception::Code::kIllegalInstruction:
      kind = "illegal instruction";
      code = 0xC000001D;  // EXCEPTION_ILLEGAL_INSTRUCTION
      break;
    default:
      return false;
  }
  if (reporting || reports.fetch_add(1) >= kMaxReports) return false;
  reporting = true;
  Report(kind, code, ex->code() == rex::arch::Exception::Code::kAccessViolation ? ex->fault_address() : 0);
  reporting = false;
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
