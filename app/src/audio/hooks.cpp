// The hooks of the native audio layer on the title's XAudio render driver
// (render.h). The DARE mixer's driver object (built by 82149618, 28 bytes):
// +0x00 the vtable 8208C414, +0x0C the context of the XAudio client, +0x18 the
// client's handle (0 without a registration). Its slots:
// - +0x1C (82149870), the callback: releases the client (XAudioUnregister-
//   RenderDriverClient) and, with a callback, registers {callback, context}
//   (XAudioRegisterRenderDriverClient). The title registers 8215AEE8, which
//   runs the DARE tick (82150170) whatever its argument.
// - +0x18 (82149800), the submission: calls slot +0x20 (a counter of 6144
//   bytes per frame) with {u64 at buffer+0x14, u32 at buffer+0x1C} on its
//   stack, then XAudioSubmitRenderDriverFrame with the frame the u32 names.
// Those two are the only callers of the render imports; the destructor
// (82149728) releases the client only when +0x18 holds one. The native layer
// keeps the client out of the SDK (+0x18 stays 0), so none is called.
//
// The music: the game's volumes (its Options: 0 to 10) reach the DARE through
// 826251A0 (sound manager, group, linear volume in f1), which stores the
// group's level (20 log10, -96 to 0 dB) and sets the group's bus under the
// sound mutex. The music is group 0. The attenuation scales the volume the
// game sets for it; once a tick of the DARE (821502C8, the voice categories'
// update) a moving fade sets it again.
#include "audio/diagnostics.h"
#include "audio/game_volumes.h"
#include "audio/render.h"
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <mutex>
#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>

extern "C" void __imp__sub_82149870(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82149800(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_826251A0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_821502C8(PPCContext& __restrict, uint8_t*);

namespace audio {
namespace {
constexpr uint32_t kSetCallback = 0x82149870;
constexpr uint32_t kContext = 0x0C, kHandle = 0x18, kCounterSlot = 0x20;
constexpr uint32_t kSubmitFrame = 0x70;  // The submission's stack frame: the pair at +0x50, the frame at +0x58.
constexpr uint32_t kMusicGroup = 0;

// The music volume the game last set (-1 before), and its sound manager.
std::atomic<uint32_t> music_manager{0};
std::atomic<float> music_volume{-1.0f};

uint32_t Get32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, 4);
  return std::byteswap(value);
}
void Put32(uint8_t* base, uint32_t address, uint32_t value) {
  value = std::byteswap(value);
  std::memcpy(base + address, &value, 4);
}

// Serializes the registration (the title's, or a move between the layers)
// with the submissions, so a submission never reaches an SDK client that is
// being released.
std::mutex switch_mutex;
// The original registration run by MoveSdkClient through the dispatcher.
thread_local bool forwarding = false;

// A virtual call of the guest on the current thread, with the caller's
// registers restored afterwards.
void CallSlot(PPCContext& ctx, uint8_t* base, uint32_t object, uint32_t slot, uint32_t sp, uint32_t r4) {
  auto* function = rex::runtime::ResolveIndirectFunction(Get32(base, Get32(base, object) + slot));
  if (!function) return;
  std::array<uint8_t, sizeof(PPCContext)> saved;
  std::memcpy(saved.data(), &ctx, sizeof(PPCContext));
  ctx.r1.u64 = sp;
  ctx.r3.u64 = object;
  ctx.r4.u64 = r4;
  function(ctx, base);
  std::memcpy(&ctx, saved.data(), sizeof(PPCContext));
}
}  // namespace

bool MoveSdkClient(const Client& client, bool sdk) {
  auto* kernel = rex::system::kernel_state();
  const uint8_t* base = kernel->memory()->virtual_membase();
  std::lock_guard lock(switch_mutex);
  if ((Get32(base, client.driver + kHandle) != 0) != sdk) {
    uint64_t args[] = {client.driver, sdk ? client.callback : 0u};
    forwarding = true;
    kernel->function_dispatcher()->Execute(rex::runtime::ThreadState::Get(), kSetCallback, args, 2);
    forwarding = false;
  }
  return Get32(base, client.driver + kHandle) != 0;
}
}  // namespace audio

REX_HOOK_RAW(sub_82149870) {
  using namespace audio;
  if (forwarding) {
    __imp__sub_82149870(ctx, base);
    return;
  }
  std::lock_guard lock(switch_mutex);
  const Client client{ctx.r3.u32, ctx.r4.u32, Get32(base, ctx.r3.u32 + kContext)};
  if (!NativeRequested()) {
    __imp__sub_82149870(ctx, base);
    Register(client, Get32(base, client.driver + kHandle) ? Owner::kSdk : Owner::kNone);
    return;
  }
  // A client the SDK holds (registered while audio_layer was "sdk") is
  // released first, as the original does; a failed release keeps it there.
  if (Get32(base, client.driver + kHandle)) {
    const uint64_t callback = ctx.r4.u64;
    ctx.r4.u64 = 0;
    __imp__sub_82149870(ctx, base);
    ctx.r4.u64 = callback;
    if (ctx.r3.s32 < 0) return;
  }
  Register(client, Owner::kNative);
  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(sub_82149800) {
  using namespace audio;
  const uint32_t driver = ctx.r3.u32;
  const uint32_t buffer = ctx.r4.u32 ? ctx.r4.u32 - 8 : 0;
  {
    std::lock_guard lock(switch_mutex);
    if (Get32(base, driver + kHandle)) {  // The SDK's client.
      if (buffer) diagnostics::Record(base + Get32(base, buffer + 0x1C));
      ++diagnostics::Get().sdk_frames;
      __imp__sub_82149800(ctx, base);
      return;
    }
  }
  if (!buffer) {
    ctx.r3.u64 = 0;
    return;
  }
  // The original's body without the import.
  const uint32_t sp = ctx.r1.u32 - kSubmitFrame;
  Put32(base, sp, ctx.r1.u32);
  std::memcpy(base + sp + 0x50, base + buffer + 0x14, 8);
  std::memcpy(base + sp + 0x58, base + buffer + 0x1C, 4);
  CallSlot(ctx, base, driver, kCounterSlot, sp, sp + 0x50);
  if (const uint32_t frame = Get32(base, sp + 0x58)) Deliver(base + frame);
  ctx.r3.u64 = 0;
}

// (sound manager, group, volume in f1): the music under the attenuation.
REX_HOOK_RAW(sub_826251A0) {
  using namespace audio;
  if (ctx.r4.u32 == kMusicGroup) {
    music_manager = ctx.r3.u32;
    music_volume = float(ctx.f1.f64);
    REXLOG_INFO("[narutorise] Audio: the game sets the music volume to {:.2f}", ctx.f1.f64);
    ctx.f1.f64 *= MusicFade();
  }
  __imp__sub_826251A0(ctx, base);
}

// (mixer, flag): once a tick, under the sound mutex: the game's volumes changed
// in the settings (game_volumes.h), and a moving fade sets the music again.
REX_HOOK_RAW(sub_821502C8) {
  using namespace audio;
  __imp__sub_821502C8(ctx, base);
  game_volumes::ApplyPending(ctx, base);
  const uint32_t manager = music_manager.load();
  const float volume = music_volume.load();
  if (!StepMusicFade() || !manager || volume < 0.0f) return;
  std::array<uint8_t, sizeof(PPCContext)> saved;
  std::memcpy(saved.data(), &ctx, sizeof(PPCContext));
  ctx.r3.u64 = manager;
  ctx.r4.u64 = kMusicGroup;
  const float fade = MusicFade();
  ctx.f1.f64 = double(volume * fade);
  __imp__sub_826251A0(ctx, base);
  std::memcpy(&ctx, saved.data(), sizeof(PPCContext));
  if (fade == 0.0f || fade == 1.0f)
    REXLOG_INFO("[narutorise] Audio: the music {} (volume {:.2f})", fade == 0.0f ? "faded out" : "is back", volume);
}
