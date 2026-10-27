#pragma once

// The guest device owner of the D3D boundary (DeviceOwner below).
#include "gpu/d3d/d3d.h"
#include "gpu/backend/dispatcher.h"
#include "gpu/backend/failure_channel.h"
#include "gpu/d3d/init/initializer_transaction.h"
#include "gpu/d3d/init/initializer_abi.h"
#include "gpu/d3d/init/video_transaction.h"
#include "gpu/d3d/init/video_abi.h"
#include "gpu/d3d/init/default_transaction.h"
#include "gpu/d3d/init/default_constants.h"
#include "gpu/diagnostics/profile.h"
#include "gpu/diagnostics/diagnostics.h"
#include "gpu/common/guest_memory.h"
#include <bit>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>
#include <rex/system/xmemory.h>

extern "C" void __imp__RtlTryEnterCriticalSection(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__RtlLeaveCriticalSection(PPCContext& __restrict, uint8_t*);

namespace gpu {

// The device creation. The original 8213ECE8 body runs on the device that
// 820E6B00 just allocated. Locks, terminate registration, global publication,
// XConfig, DDB0, clock, 43FF8 and the HSIO gate stay original. Its serializing
// internals are replaced by the accepted CPU projections (483D0, 3E8F0, E610,
// 280B0 dirty) or omitted (ring FA80, workers 480D0, PM4 cache 31428, F608 and
// EDRAM 445D8). Cursor +48 above limit +56 sends every inline writer to the
// guarded F608. The hooks that drive it: d3d/device.cpp.

class DeviceOwner final {
 public:
  enum class Step { Idle, Entered, Arena, Workers, Cache, Initializer, Video, Defaults,
                    Memory, Flush, Callbacks, Edram, Dirty, Returned, Live };
  static constexpr uint32_t kCursor = 0x80000000u;  // > limit 0 for any N < 2^31
  static constexpr uint32_t kColdCounter = 3;       // FA80 cold store, as in the accepted arena

  bool Enter(PPCContext& ctx, uint8_t* base) {
    auto* ts = rex::runtime::ThreadState::Get();
    auto* thread = rex::system::XThread::GetCurrentThread();
    if (step_ != Step::Idle || !ts || !thread || ts->context() != &ctx ||
        ts->memory()->virtual_membase() != base || ctx.lr != 0x820E6BB0 ||
        !ctx.r3.u32 || (ctx.r3.u32 & 127) || !ctx.r4.u32) return Reject("ECE8-entry-signature");
    memory_ = ts->memory();
    device_ = ctx.r3.u32;
    params_ = ctx.r4.u32;
    thread_ = ts->thread_id();
    cpu_ = int(thread->active_cpu());
    auto* heap = memory_->LookupHeap(device_);
    rex::memory::HeapAllocationInfo info{};
    if (!heap || heap->heap_type() != rex::memory::HeapType::kGuestVirtual ||
        !heap->QueryRegionInfo(device_, &info) || !(info.state & rex::memory::kMemoryAllocationCommit) ||
        (info.protect & (rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite)) !=
            (rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite) ||
        uint64_t(info.base_address) + info.region_size < uint64_t(device_) + kDeviceBytes)
      return Reject("ECE8-device-not-owned-virtual-rw");
    uint32_t flags = 0, refcount = 0, cursor = 1, limit = 1;
    if (!Word(22280, flags) || flags != 0x0C000001 || !Word(60, refcount) || refcount != 1 ||
        !Word(48, cursor) || !Word(56, limit) || cursor || limit)
      return Reject("ECE8-wrapper-state-variant");
    step_ = Step::Entered;
    host::Record("native-d3d-device-enter", fmt::format(
        "\"device\":{},\"params\":{},\"lr\":{},\"guest_thread\":{},\"cpu\":{},\"flags22280\":{},"
        "\"refcount60\":1,\"original_body\":true", device_, params_, ctx.lr, thread_, cpu_, flags));
    return true;
  }

  // Row 06: no ring, read-pointer writeback, identifier or staging.
  bool Arena(PPCContext& ctx) {
    if (!At(ctx, Step::Entered, 0x8213EDA8) || ctx.r4.u32 != params_ + 72) return Reject("FA80-context");
    for (uint32_t at : {48u, 52u, 56u, 10896u, 10900u, 10908u, 14820u, 14824u}) {
      uint32_t value = 1;
      if (!Word(at, value) || value) return Reject("FA80-cold-field-nonzero");
    }
    // 10908 is the submission counter that D5D0, AC80, 31950 and 31C08 stamp
    // into unbound resource headers (+8); zero selects their deferred-list path
    // instead. Keep the original cold value.
    if (!Store(48, kCursor) || !Store(10908, kColdCounter)) return Reject("FA80-cursor-or-counter-store");
    // Own 96-byte writeback block at +10896, as FA80 allocates it.
    // +0 is the completed fence, cold value 10908 - 2; +4 (ring read pointer)
    // and +60 stay zero without a ring.
    writeback_ = memory_->SystemHeapAlloc(kWritebackBytes, 32);
    std::array<uint8_t, kWritebackBytes> zero{};
    if (!writeback_ || !WriteVerify(writeback_, zero.data(), zero.size()) ||
        !GuestStore(writeback_, kColdCounter - 2) || !Store(10896, writeback_))
      return Reject("FA80-writeback-block");
    // Own 32-byte scratch block at +10900, zeroed as FA80 does. The
    // own interrupt source writes SCRATCH_REG0/4/5 (+0, +16, +20) before source
    // 1; 82138358/82138258 store the flip handshake at +4. Packets addressing
    // it only reach the discard buffer.
    scratch_ = memory_->SystemHeapAlloc(kScratchBytes, 32);
    if (!scratch_ || !WriteVerify(scratch_, zero.data(), kScratchBytes) || !Store(10900, scratch_))
      return Reject("FA80-scratch-block");
    ctx.r3.u64 = 0;
    step_ = Step::Arena;
    host::Record("native-d3d-device-ring-omitted", fmt::format(
        "\"device\":{},\"cursor48\":{},\"limit56\":0,\"writeback10896\":{},\"completed\":{},"
        "\"counter10908\":{},\"scratch10900\":{},\"ring\":false,\"inline_writers_reach\":\"guarded-F608\","
        "\"status\":0", device_, kCursor, writeback_, kColdCounter - 2, kColdCounter, scratch_));
    return true;
  }
  // Rows 07/08: no legacy command workers; the 200 block stays unallocated.
  bool Workers(PPCContext& ctx) {
    uint32_t block = 1;
    if (!At(ctx, Step::Arena, 0x8213EDB8) || !Word(11796, block) || block) return Reject("480D0-context");
    ctx.r3.u64 = 1;
    step_ = Step::Workers;
    host::Record("native-d3d-device-workers-omitted", fmt::format(
        "\"device\":{},\"block11796\":0,\"threads_created\":0,\"status\":1", device_));
    return true;
  }
  // Row 09: no PM4 command cache; 13764 and its record words stay zero.
  bool Cache(PPCContext& ctx) {
    uint32_t cache = 1;
    if (!At(ctx, Step::Workers, 0x8213EDC8) || !Word(13764, cache) || cache) return Reject("31428-context");
    ctx.r3.u64 = 1;
    step_ = Step::Cache;
    host::Record("native-d3d-device-cache-omitted", fmt::format(
        "\"device\":{},\"cache13764\":0,\"status\":1", device_));
    return true;
  }

  // Row 10: accepted 70-word CPU projection and own frontend services.
  bool Initializer(PPCContext& ctx, Dispatcher* dispatcher) {
    if (!At(ctx, Step::Cache, 0x8213EDE4) || !dispatcher) return Reject("483D0-context");
    uint32_t ring = 1, edge = 1;
    if (!Word(21540, ring) || !Word(21544, edge) || ring != UINT32_MAX || edge != UINT32_MAX)
      return Reject("483D0-caller-sentinels");
    const auto entry = gpu::CaptureInitializerAbi(ctx);
    auto authority = [&] { return Owns(); };
    auto read = [&](uint32_t at, std::array<uint8_t, 4>& bytes) {
      return ReadVirtual(memory_, device_ + at, bytes.data(), 4);
    };
    auto write = [&](uint32_t at, const std::array<uint8_t, 4>& bytes) {
      return WriteVerify(device_ + at, bytes.data(), 4);
    };
    if (!initializer_.Capture(authority, read)) return Reject("483D0-capture");
    dispatcher_ = dispatcher;
    std::string evidence;
    frontend_attempted_ = true;
    if (!dispatcher->InitializeFrontend(kGeneration, evidence)) return Reject("483D0-frontend");
    if (!initializer_.Commit(authority, read, write) ||
        !dispatcher->VerifyFrontend(kGeneration, evidence) ||
        !gpu::InitializerAbiMatches(entry, ctx)) return Reject("483D0-commit");
    step_ = Step::Initializer;
    host::Record("native-d3d-device-initializer", fmt::format(
        "\"device\":{},\"cpu_words\":70,\"frontend\":{},\"void_abi\":true,\"serialization\":false",
        device_, evidence));
    return true;
  }

  // Rows 11/12: accepted mode/flags/gamma/notification projection.
  bool Video(PPCContext& ctx, uint8_t* base) {
    if (!At(ctx, Step::Initializer, 0x8213EDF0) || ctx.r4.u32 != params_) return Reject("3E8F0-context");
    const auto entry = gpu::CaptureInitializerAbi(ctx);
    gpu::VideoParameters parameters{};
    std::array<uint32_t, 3> slots{};
    uint32_t gamma_owner = 0, gamma_flags = 0;
    uint8_t flags = 0;
    gpu::GammaRamp previous{};
    if (!ReadVirtual(memory_, params_, parameters.data(), parameters.size()) ||
        !Word(14808, slots[0]) || !Word(14812, slots[1]) || !Word(14816, slots[2]) ||
        !Word(22252, gamma_owner) || !Word(22256, gamma_flags) ||
        !ReadVirtual(memory_, device_ + 10942, &flags, 1) || flags != 4 ||
        !ReadVirtual(memory_, device_ + 14988, previous.data(), previous.size()))
      return Reject("3E8F0-inputs");
    auto authority = [&] { return Owns(); };
    auto read = [&](uint32_t at, std::span<uint8_t> bytes) {
      return ReadVirtual(memory_, device_ + at, bytes.data(), bytes.size());
    };
    auto write = [&](uint32_t at, std::span<const uint8_t> bytes) {
      return WriteVerify(device_ + at, bytes.data(), bytes.size());
    };
    gpu::VideoPlan plan{};
    if (!video_cpu_.Capture(authority, read) || !video_services_.Initialize(kGeneration) ||
        !video_services_.Prepare(kGeneration, parameters, slots, gamma_owner, gamma_flags, flags,
                                 previous, plan) ||
        !video_cpu_.Plan(plan)) return Reject("3E8F0-profile-or-plan");
    if (!video_cpu_.Commit(authority, read, write)) return Reject("3E8F0-commit");
    if (!OrFlagsUnderLock(ctx, base, 4, 0x14)) return Reject("3E8F0-flags10942");
    if (!video_services_.Notify(kGeneration, 1, plan.notification)) return Reject("3E8F0-notify");
    ctx.r3.u64 = 1;
    if (!gpu::InitializerAbiMatches(gpu::VideoReturnAbi(entry), ctx))
      return Reject("3E8F0-status-abi");
    step_ = Step::Video;
    host::Record("native-d3d-device-video", fmt::format(
        "\"device\":{},\"cpu_bytes\":1740,\"flags10942\":20,\"slots\":[{},{},{}],\"status\":1,"
        "\"resource_creation\":false,\"serialization\":false", device_, slots[0], slots[1], slots[2]));
    return true;
  }

  // Rows 13/14: accepted defaults, dispatch and D5D0 unbinding projection.
  bool Defaults(PPCContext& ctx) {
    if (!At(ctx, Step::Video, 0x8213EE00) || ctx.r1.u32 < 85) return Reject("E610-context");
    const auto entry = gpu::CaptureInitializerAbi(ctx);
    gpu::DefaultBootstrapInput input{};
    input.device_address = device_;
    uint8_t inherited = 0;
    if (!ReadVirtual(memory_, device_, input.cpu.cpu.data(), kDeviceBytes) ||
        !ReadVirtual(memory_, ctx.r1.u32 - 85, &inherited, 1) || !TablesAndConstantsMatch())
      return Reject("E610-inputs");
    input.stack91 = inherited;
    if (input.cpu.cpu[10940] != 0 || input.cpu.Word(22280) != 0x0C000001)
      return Reject("E610-signature");
    auto authority = [&] { return Owns(); };
    auto read = [&](uint32_t at, std::span<uint8_t> bytes) {
      return uint64_t(at) + bytes.size() <= kDeviceBytes &&
          ReadVirtual(memory_, device_ + at, bytes.data(), bytes.size());
    };
    auto write = [&](uint32_t at, std::span<const uint8_t> bytes) {
      return uint64_t(at) + bytes.size() <= kDeviceBytes && WriteVerify(device_ + at, bytes.data(), bytes.size());
    };
    if (!defaults_.Capture(kGeneration, input, authority, read) ||
        !defaults_.Commit(kGeneration, authority, read, write) ||
        !gpu::InitializerAbiMatches(entry, ctx)) return Reject("E610-commit");
    step_ = Step::Defaults;
    host::Record("native-d3d-device-defaults", fmt::format(
        "\"device\":{},\"cpu_bytes\":{},\"helper31DD8\":{},\"inherited_stack_byte\":{},\"void_abi\":true",
        device_, defaults_.plan().direct.call31DD8 ? 2044 : 2040, defaults_.plan().direct.call31DD8,
        unsigned(inherited)));
    return true;
  }

  // DDB0(0, B1800000) returns at its first test. The following original block
  // only writes into the 820008F8 object when its first word is nonzero.
  bool Memory(PPCContext& ctx) {
    uint32_t root = 0, cell = 1;
    // r3 is the zero argument here, not the device; r31 still holds it.
    if (!Owns() || step_ != Step::Defaults || ctx.lr != 0x8213EE0C || ctx.r31.u32 != device_ ||
        ctx.r3.u32 || !(ctx.r4.u32 & 0x80000000u) ||
        !ReadVirtual(memory_, 0x820008F8, &root, 4)) return Reject("DDB0-context");
    root = Swap(root);
    if (root && !GuestWord(root, cell)) return Reject("DDB0-external-root");
    if (root && cell) return Reject("DDB0-external-record-present");
    step_ = Step::Memory;
    host::Record("native-d3d-device-ddb0", fmt::format(
        "\"device\":{},\"r3\":0,\"r4\":{},\"original_noop\":true,\"root820008F8\":{},\"record\":0",
        device_, ctx.r4.u32, root));
    return true;
  }
  // The ring flush of the creation: none. The caller discards r3.
  bool Flush(PPCContext& ctx) {
    uint32_t cursor = 0;
    if (!At(ctx, Step::Memory, 0x8213EE60) || !Word(48, cursor) || cursor != kCursor)
      return Reject("F608-context");
    ctx.r3.u64 = cursor;
    step_ = Step::Flush;
    host::Record("native-d3d-device-flush-omitted", fmt::format(
        "\"device\":{},\"cursor48\":{},\"submission\":false", device_, cursor));
    return true;
  }
  // Row 17: both external callback chains must be empty; then the original
  // only clears 82C0F1A4.
  bool Callbacks(PPCContext& ctx) {
    uint32_t root86c = 0, root8f8 = 0, cell86c = 1, cell8f8 = 1;
    if (!At(ctx, Step::Flush, 0x8213EE98) || !GuestWord(0x8200086C, root86c) ||
        !GuestWord(0x820008F8, root8f8) || (root86c && !GuestWord(root86c, cell86c)) ||
        (root8f8 && !GuestWord(root8f8, cell8f8))) return Reject("43FF8-context");
    if ((root86c && cell86c) || (root8f8 && cell8f8)) return Reject("43FF8-callback-present");
    step_ = Step::Callbacks;
    host::Record("native-d3d-device-callbacks", fmt::format(
        "\"device\":{},\"root8200086C\":{},\"root820008F8\":{},\"callbacks_invoked\":0,"
        "\"original_clears_82C0F1A4\":true", device_, root86c, root8f8));
    return true;
  }
  // Row 18: EDRAM retraining is legacy hardware; original returns 1.
  bool Edram(PPCContext& ctx) {
    if (!At(ctx, Step::Callbacks, 0x8213EEA8) || ctx.r4.u32 != 1 || ctx.r5.u32 != 1)
      return Reject("445D8-context");
    ctx.r3.u64 = 1;
    step_ = Step::Edram;
    host::Record("native-d3d-device-edram-omitted", fmt::format("\"device\":{},\"status\":1", device_));
    return true;
  }
  // The dirty-state step with r4 == 0: 82128270 and the packets only use cursor +48;
  // 10916 is stored back unchanged; the CPU effect is all-ones +0..+39.
  bool Dirty(PPCContext& ctx) {
    if (!At(ctx, Step::Edram, 0x8213EEB4) || ctx.r4.u32) return Reject("280B0-context");
    std::array<uint8_t, 40> ones{};
    ones.fill(0xFF);
    if (!WriteVerify(device_, ones.data(), ones.size())) return Reject("280B0-dirty");
    step_ = Step::Dirty;
    host::Record("native-d3d-device-dirty", fmt::format(
        "\"device\":{},\"dirty_bytes\":40,\"word10916_unchanged\":true,\"serialization\":false", device_));
    return true;
  }
  bool AdmitHsio(PPCContext& ctx) {
    return Owns() && step_ == Step::Dirty && ctx.lr == 0x8213EEC8;
  }

  // Row 20 and natural return to 820E6B00.
  bool Returned(PPCContext& ctx) {
    uint32_t hsio = 1, sentinel = 0;
    if (!Owns() || step_ != Step::Dirty || ctx.r3.u32 != 1 || ctx.lr != 0x820E6BB0 ||
        !GuestWord(0x82C0F19C, hsio) || hsio) return Reject("ECE8-return");
    for (uint32_t i = 0; i < 41; ++i)
      if (!Word(21680 + 4 * i, sentinel) || sentinel != UINT32_MAX) return Reject("ECE8-sentinels");
    owner_ = ctx.r30.u32;
    step_ = Step::Returned;
    host::Record("native-d3d-device-return", fmt::format(
        "\"device\":{},\"status\":1,\"lr\":{},\"owner\":{},\"hsio_failed\":0,\"sentinels21680\":41",
        device_, ctx.lr, owner_));
    return true;
  }
  // BEFORE 8212ADA0/LR 820E6D18: the wrapper published the device at owner+16.
  bool Checkpoint(PPCContext& ctx) {
    uint32_t published = 0;
    if (step_ != Step::Returned || !Owns() || ctx.lr != 0x820E6D18 || !owner_ ||
        !GuestWord(owner_ + 16, published) || published != device_) return Reject("ADA0-checkpoint");
    step_ = Step::Live;
    live_.store(true, std::memory_order_release);
    host::Record("native-d3d-device-checkpoint", fmt::format(
        "\"device\":{},\"owner\":{},\"published_owner16\":{},\"lr\":{},\"r3\":{},\"r4\":{},"
        "\"guest_thread\":{},\"cpu\":{}", device_, owner_, published, ctx.lr, ctx.r3.u32, ctx.r4.u32,
        thread_, cpu_));
    return true;
  }
  // ADA0 (r4 slot, r5 header) and B108 (r4 header) run the original
  // CPU setters; this records the binding identity for the own surfaces.
  // Called from the main and render threads.
  bool Bind(PPCContext& ctx, bool depth) {
    if (!live_.load(std::memory_order_acquire) || ctx.r3.u32 != device_) return Reject(depth ? "B108-device" : "ADA0-device");
    const uint32_t slot = depth ? 0 : ctx.r4.u32, header = depth ? ctx.r4.u32 : ctx.r5.u32;
    if (slot > 3) return Reject("ADA0-slot");
    std::array<uint8_t, 48> bytes{};
    if (header && !ReadVirtual(memory_, header, bytes.data(), bytes.size())) return Reject("binding-header-unreadable");
    auto* ts = rex::runtime::ThreadState::Get();
    std::lock_guard lock(live_mutex_);
    const auto call = ++binds_[depth];
    if (Diagnostics() && (!profile::QuietRecords() || call <= profile::kQuietRecords))  // Capped in the measurement.
      host::Record("native-d3d-device-binding", fmt::format(
        "\"binding\":\"{}\",\"call\":{},\"slot\":{},\"header\":{},\"header_hex\":\"{}\",\"lr\":{},"
        "\"guest_thread\":{},\"original_cpu_setter\":true,\"own_surface\":\"4.1.A3\"",
        depth ? "depth" : "target", call, slot, header, header ? Hex(bytes.data(), bytes.size()) : "",
        ctx.lr, ts ? ts->thread_id() : 0));
    return true;
  }
  // 82128270 only serializes the window/scissor packet through cursor +48.
  // Its inputs stay in the device (10436/10440, 12668..12680, 12740 list),
  // where the own draw consumes them; no emission and no cursor change.
  bool Scissor(PPCContext& ctx) {
    if (!live_.load(std::memory_order_acquire) || ctx.r3.u32 != device_) return Reject("28270-device");
    std::lock_guard lock(live_mutex_);
    if (++scissors_ <= 8 && Diagnostics()) {
      auto* ts = rex::runtime::ThreadState::Get();
      host::Record("native-d3d-device-scissor", fmt::format(
          "\"call\":{},\"rect\":[{},{},{},{}],\"lr\":{},\"guest_thread\":{},\"emitted\":false",
          scissors_, int32_t(ctx.r4.u32), int32_t(ctx.r5.u32), int32_t(ctx.r6.u32), int32_t(ctx.r7.u32),
          ctx.lr, ts ? ts->thread_id() : 0));
    }
    return true;
  }
  // Owner id -1 is the original "no owner" value of 820E7428. Restore only an
  // unchanged -1, so a concurrent ownership change is never overwritten.
  bool BeginLock(PPCContext& ctx, uint32_t& saved) {
    uint32_t owner = 0, current = 0, header0 = 0;
    auto* ts = rex::runtime::ThreadState::Get();
    if (!live_.load(std::memory_order_acquire) || !ts || ts->memory() != memory_ || !ctx.r3.u32 ||
        !Word(10888, owner) || !GuestWord(ctx.r3.u32, header0)) return Reject("D950-context");
    uint32_t kthread = 0;
    if (!GuestWord(ctx.r13.u32 + 256, kthread) || !GuestWord(kthread + 332, current)) return Reject("D950-thread-id");
    saved = owner == current ? owner : 0;
    if (saved && !Store(10888, UINT32_MAX)) return Reject("D950-owner-store");
    std::lock_guard lock(live_mutex_);
    if (++locks_ <= 8 && Diagnostics())
      host::Record("native-d3d-device-lock", fmt::format(
          "\"call\":{},\"header\":{},\"header0\":{},\"r4\":{},\"flags\":{},\"bytes\":{},\"lr\":{},"
          "\"guest_thread\":{},\"owner_thread\":{},\"owner_path_redirected\":{}",
          locks_, ctx.r3.u32, header0, ctx.r4.u32, ctx.r10.u32, ctx.r9.u32, ctx.lr, ts->thread_id(),
          bool(saved), bool(saved)));
    return true;
  }
  bool EndLock(PPCContext& ctx, uint32_t saved) {
    uint32_t owner = 0;
    if (!saved) return true;
    if (!Word(10888, owner)) return Reject("D950-owner-read");
    if (owner == UINT32_MAX && !Store(10888, saved)) return Reject("D950-owner-restore");
    return true;
  }
  uint32_t device() const { return device_; }
  uint32_t scratch() const { return scratch_; }
  // D3DDevice_BlockOnFence with the console protocol: fence f is
  // pending while (10908 - f) < (10908 - completed). For the open batch
  // (f == 10908) the original kicks first, which advances 10908 by 2; here the
  // own submissions are that batch.
  // Frame ends are published in order (PublishFrame): the own frame
  // completion thread publishes each frame fence once its lot completed on
  // the own queue. A fence
  // covered by a pending frame end waits for those publications; any other
  // fence waits for the own queue (the drain, never under the fence lock, so
  // the interrupt source is not held by a GPU wait) and is published up to the
  // fence before the first pending frame end. failed() ends a wait (posted
  // failure, stop); a wait without publication for kGateTimeout fails too.
  enum class FenceResult { Done, Skipped, Waited, Failed };
  template <typename Drain, typename Failed>
  FenceResult BlockOnFence(uint32_t fence, Drain&& drain, Failed&& failed, std::string& fields) {
    std::unique_lock lock(fence_mutex_);
    uint32_t counter = 0, completed = 0, suspended = 0;
    if (!live_.load(std::memory_order_acquire) || !writeback_ || !Word(10908, counter) ||
        !GuestWord(writeback_, completed) || !Word(13216, suspended)) return FenceResult::Failed;
    const uint32_t before = counter, before_completed = completed;
    const char* path = "none";
    const size_t gates_before = gates_.size();
    auto record = [&](const char* result) {
      fields = fmt::format("\"fence\":{},\"counter_before\":{},\"completed_before\":{},\"counter\":{},"
                           "\"completed\":{},\"result\":\"{}\",\"path\":\"{}\",\"pending_frame_ends\":{}", fence,
                           before, before_completed, counter, completed, result, path, gates_before);
    };
    if (Done(counter, fence, completed)) { record("done"); return FenceResult::Done; }
    if (fence == counter) {
      if (suspended) { record("skipped-13216"); return FenceResult::Skipped; }  // As the original.
      counter += 2;
      if (!Store(10908, counter)) return FenceResult::Failed;
    }
    const bool drained = gates_.empty() || !NotAfter(counter, fence, gates_.back());
    const uint32_t reach = counter - 2;  // Closed on the own queue once the drain returns.
    if (drained) {
      path = "drain";
      ++drain_waits_;
      lock.unlock();
      const bool idle = drain();
      lock.lock();
      if (!idle) return FenceResult::Failed;
    } else {
      path = "frame-end";
      ++frame_end_waits_;
    }
    // This includes FIFO translation and CPU pipeline compilation before
    // submission. The backend separately bounds actual GPU completion.
    auto deadline = std::chrono::steady_clock::now() + kGateTimeout;
    for (;;) {
      if (!Word(10908, counter) || !GuestWord(writeback_, completed)) return FenceResult::Failed;
      if (drained) {
        // The GPU passes everything drained, up to the first pending frame end.
        uint32_t value = reach;
        if (!gates_.empty() && NotAfter(counter, gates_.front(), value)) value = gates_.front() - 2;
        if (Later(counter, value, completed) != completed) {
          completed = value;
          if (!GuestStore(writeback_, completed)) return FenceResult::Failed;
        }
      }
      if (Done(counter, fence, completed)) break;
      if (released_ || failed()) return FenceResult::Failed;
      const uint64_t published = frame_publications_;
      fence_cv_.wait_for(lock, std::chrono::milliseconds(5));
      if (frame_publications_ != published) deadline = std::chrono::steady_clock::now() + kGateTimeout;
      else if (std::chrono::steady_clock::now() > deadline) return FenceResult::Failed;
    }
    record("waited");
    return FenceResult::Waited;
  }
  // End of D3DDevice_Swap: the frame batch closes as 8212F2C0 does
  // (fence = 10908, then 10908 += 2). close_lot(fence) closes the own lot
  // without waiting; the frame end stays pending until the frame completion
  // thread publishes it, as the console GPU writes it to +0 of the writeback
  // block after the WAIT_REG_MEM of 821384F0.
  template <typename CloseLot>
  bool CloseFrameBatch(CloseLot&& close_lot, uint32_t& fence, std::string& fields) {
    std::lock_guard lock(fence_mutex_);
    uint32_t counter = 0, completed = 0;
    if (!live_.load(std::memory_order_acquire) || !writeback_ || !Word(10908, counter) ||
        !GuestWord(writeback_, completed) || gates_.size() >= kGatesMax) return false;
    if (!close_lot(counter) || !Store(10908, counter + 2)) return false;
    gates_.push_back(counter);
    fence = counter;
    fields = fmt::format("\"frame_fence\":{},\"completed\":{},\"counter\":{},\"pending_frame_ends\":{}", counter,
                         completed, counter + 2, gates_.size());
    return true;
  }
  // The lot of this frame fence completed on the own queue: the frame fence
  // becomes the completed value. Frame ends are published in order. The store
  // is authorized by the session's failure channel inside the fence lock (the
  // lock order fence lock, then channel: the Swap holds the fence lock while it
  // posts the lot close, which can wait for the owner).
  FramePublication PublishFrame(uint32_t fence, FailureChannel& failures) {
    std::lock_guard lock(fence_mutex_);
    uint32_t counter = 0, completed = 0;
    if (!live_.load(std::memory_order_acquire) || !writeback_ || gates_.empty() || gates_.front() != fence ||
        !Word(10908, counter) || !GuestWord(writeback_, completed)) return FramePublication::kFailed;
    bool stored = false;
    if (!failures.Authorize([&] {
          gates_.pop_front();
          stored = GuestStore(writeback_, Later(counter, fence, completed));
        }))
      return FramePublication::kRefused;
    if (!stored) return FramePublication::kFailed;
    ++frame_publications_;
    fence_cv_.notify_all();
    return FramePublication::kPublished;
  }
  std::string FenceSummary() {
    std::lock_guard lock(fence_mutex_);
    return fmt::format("{{\"pending_frame_ends\":{},\"frame_publications\":{},\"drain_waits\":{},"
                       "\"frame_end_waits\":{}}}", gates_.size(), frame_publications_, drain_waits_,
                       frame_end_waits_);
  }
  void NoteSubmission() {
    std::lock_guard lock(live_mutex_);
    ++own_submissions_;
  }
  uint64_t NoteFence() {
    std::lock_guard lock(live_mutex_);
    return ++fences_;
  }
  void RecordScissors() {
    std::lock_guard lock(live_mutex_);
    host::Record("native-d3d-device-scissor-total", fmt::format(
        "\"calls\":{},\"target_bindings\":{},\"depth_bindings\":{},\"locks\":{},\"own_submissions\":{},"
        "\"fence_waits\":{}", scissors_, binds_[0], binds_[1], locks_, own_submissions_, fences_));
  }
  void ReleaseServices() {
    {
      std::lock_guard lock(fence_mutex_);  // Fence waiters end without publication.
      released_ = true;
      fence_cv_.notify_all();
    }
    std::string evidence;
    // Own submissions must be complete before the frontend can be released.
    if (frontend_attempted_ && dispatcher_) {
      dispatcher_->Run([](Engine& engine, std::string& out) { return engine.WaitIdle(out); }, evidence);
      std::string idle = evidence;
      if (!dispatcher_->ReleaseFrontend(kGeneration, evidence))
        host::Record("native-d3d-device-frontend-release-refused", fmt::format(
            "\"idle\":{},\"release\":{}", idle, evidence));
    }
    if (video_services_.Matches(kGeneration)) video_services_.Release(kGeneration);
    // The writeback and scratch blocks outlive the device pointer only until
    // process exit. Callers quiesce the own interrupt source first.
    if (writeback_ && Store(10896, 0)) {
      memory_->SystemHeapFree(writeback_);
      writeback_ = 0;
    }
    if (scratch_ && Store(10900, 0)) {
      memory_->SystemHeapFree(scratch_);
      scratch_ = 0;
    }
  }
  bool live() const { return live_.load(std::memory_order_acquire); }
  bool inside() const { return step_ != Step::Idle && step_ < Step::Returned; }
  const char* failure() const { return failure_ ? failure_ : "device-owner-unknown"; }

 private:
  static constexpr uint64_t kGeneration = 1;
  static constexpr uint32_t kDeviceBytes = 22400;
  static constexpr uint32_t kWritebackBytes = 96;
  static constexpr uint32_t kScratchBytes = 32;
  static constexpr size_t kGatesMax = 8;  // Pending frame ends (the Swap keeps at most two).
  // One budget for both APIs: a cold driver cache can spend tens of seconds
  // compiling the first frames' pipelines, slower on older compilers. A lost
  // device fails its own wait in the backend, not this one.
  static constexpr auto kGateTimeout = std::chrono::seconds(60);
  static uint32_t Swap(uint32_t v) { return std::byteswap(v); }
  // Fence order relative to the counter 10908, as the console compares them.
  static bool NotAfter(uint32_t counter, uint32_t a, uint32_t b) { return counter - a >= counter - b; }
  static bool Done(uint32_t counter, uint32_t fence, uint32_t completed) { return NotAfter(counter, fence, completed); }
  static uint32_t Later(uint32_t counter, uint32_t a, uint32_t b) { return counter - a < counter - b ? a : b; }
  bool Owns() const {
    auto* ts = rex::runtime::ThreadState::Get();
    return memory_ && ts && ts->thread_id() == thread_ && ts->memory() == memory_;
  }
  bool At(const PPCContext& ctx, Step expected, uint32_t lr) const {
    return Owns() && step_ == expected && ctx.lr == lr && ctx.r3.u32 == device_ && ctx.r31.u32 == device_;
  }
  bool GuestWord(uint32_t address, uint32_t& value) const {
    std::array<uint8_t, 4> bytes{};
    if (!ReadVirtual(memory_, address, bytes.data(), 4)) return false;
    value = BigEndianWord(bytes.data());
    return true;
  }
  bool Word(uint32_t at, uint32_t& value) const { return GuestWord(device_ + at, value); }
  bool GuestStore(uint32_t address, uint32_t value) {
    const std::array<uint8_t, 4> bytes{uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)};
    return WriteVerify(address, bytes.data(), 4);
  }
  bool Store(uint32_t at, uint32_t value) { return GuestStore(device_ + at, value); }
  // A plain store (CopyToGuest, guest_memory.h).
  bool WriteVerify(uint32_t address, const uint8_t* bytes, size_t size) {
    return address && uint64_t(address) + size <= UINT32_MAX &&
        CopyToGuest(memory_, address, bytes, size);
  }
  // Same SDK lock as the original 82143590 store; no contention is admitted.
  bool OrFlagsUnderLock(PPCContext& ctx, uint8_t* base, uint8_t expected, uint8_t replacement) {
    const auto saved = gpu::CaptureInitializerAbi(ctx);
    auto reset = [&] { std::memcpy(&ctx, saved.data(), saved.size()); };
    ctx.r3.u64 = device_ + 14928;
    __imp__RtlTryEnterCriticalSection(ctx, base);
    const bool acquired = ctx.r3.u32 == 1;
    reset();
    if (!acquired) return false;
    uint8_t before = 0;
    const bool valid = ReadVirtual(memory_, device_ + 10942, &before, 1) && before == expected &&
        WriteVerify(device_ + 10942, &replacement, 1);
    ctx.r3.u64 = device_ + 14928;
    __imp__RtlLeaveCriticalSection(ctx, base);
    reset();
    return valid && gpu::InitializerAbiMatches(saved, ctx);
  }
  bool TablesAndConstantsMatch() const {
    for (bool scalar : {true, false}) {
      const unsigned count = scalar ? 101 : 20;
      std::vector<uint8_t> bytes(count * 12);
      if (!ReadVirtual(memory_, scalar ? 0x826F37B0 : 0x826F3C70, bytes.data(), bytes.size())) return false;
      for (unsigned i = 0; i < count; ++i) {
        const auto& row = scalar ? gpu::kScalarDispatchRows[i] : gpu::kIndexedDispatchRows[i];
        if (BigEndianWord(bytes.data() + 12 * i) != row[1] || BigEndianWord(bytes.data() + 12 * i + 4) != row[0] ||
            BigEndianWord(bytes.data() + 12 * i + 8) != row[2]) return false;
      }
    }
    for (const auto& constant : gpu::kDefaultConstants) {
      std::array<uint8_t, 32> bytes{};
      if (!ReadVirtual(memory_, constant.address, bytes.data(), constant.bytes) || bytes != constant.data)
        return false;
    }
    return true;
  }
  static std::string Hex(const uint8_t* bytes, size_t size) {
    std::string out;
    for (size_t i = 0; i < size; ++i) out += fmt::format("{:02x}", bytes[i]);
    return out;
  }
  bool Reject(const char* reason) {
    if (!failure_) failure_ = reason;
    return false;
  }

  Step step_ = Step::Idle;
  rex::memory::Memory* memory_ = nullptr;
  uint32_t device_ = 0, params_ = 0, owner_ = 0, thread_ = 0;
  int cpu_ = -1;
  const char* failure_ = nullptr;
  std::atomic<bool> live_{false};
  std::mutex live_mutex_, fence_mutex_;
  // Frame fences closed and not yet flipped, in order.
  std::condition_variable fence_cv_;
  std::deque<uint32_t> gates_;
  uint64_t frame_publications_ = 0, drain_waits_ = 0, frame_end_waits_ = 0;
  bool released_ = false;
  uint32_t writeback_ = 0, scratch_ = 0;
  std::array<uint64_t, 2> binds_{};
  uint64_t scissors_ = 0, locks_ = 0, fences_ = 0, own_submissions_ = 0;
  bool frontend_attempted_ = false;
  Dispatcher* dispatcher_ = nullptr;
  gpu::InitializerCpuTransaction initializer_{};
  gpu::VideoCpuTransaction video_cpu_{};
  gpu::VideoServices video_services_{};
  gpu::DefaultCpuTransaction defaults_{};
};
}  // namespace gpu

namespace gpu {
// Set by the own D3DDevice_Swap (d3d/frame.cpp) around the display
// services it calls itself; read by AdmitExport (d3d/device.cpp).
extern thread_local bool own_swap_scope;
}  // namespace gpu
