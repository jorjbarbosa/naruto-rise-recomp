// The game's interface sounds in our menus (ui_sounds.h).
#include "audio/ui_sounds.h"
#include "audio/diagnostics.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

extern "C" void __imp__sub_825A5788(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82668A78(PPCContext& __restrict, uint8_t*);

namespace audio::ui_sounds {
namespace {
using Clock = std::chrono::steady_clock;
constexpr uint32_t kMainStepReturn = 0x826847D0;  // The manager's update in the main step (82684518).
constexpr uint32_t kOwnSound = 1, kInterfaceGroup = 15;
// By Sound: the cursor, the confirmation, a changed value, the pause menu's opening and closing.
constexpr std::array<uint32_t, 5> kKeys = {0x003B0003, 0x003B0000, 0x003B0009, 0x003B0017, 0x003B0018};
constexpr auto kStale = std::chrono::milliseconds(250);

std::mutex mutex;
std::array<std::optional<Clock::time_point>, kKeys.size()> pending;
std::atomic<bool> reported_played{false}, reported_refused{false};

// On the main thread, in the manager's update: the game's request, as its
// sound object makes it (825A5788: kind, group, sound, object, flags).
void Request(PPCContext& ctx, uint8_t* base, size_t sound) {
  const uint32_t key = kKeys[sound];
  std::array<uint8_t, sizeof(PPCContext)> saved;
  std::memcpy(saved.data(), &ctx, sizeof(PPCContext));
  ctx.r3.u64 = kOwnSound;
  ctx.r4.u64 = kInterfaceGroup;
  ctx.r5.u64 = key;
  ctx.r6.u64 = 0;  // No object: the sound is not placed in the world.
  ctx.r7.u64 = 0;
  __imp__sub_825A5788(ctx, base);
  const uint32_t handle = ctx.r3.u32;
  std::memcpy(&ctx, saved.data(), sizeof(PPCContext));
  // The first sound played, and the first the manager refused (not loaded), in the log.
  const bool played = handle != 0 && handle != 0xFFFFFFFF;
  ++(played ? diagnostics::Get().interface_sounds[sound] : diagnostics::Get().interface_refused);
  if (!(played ? reported_played : reported_refused).exchange(true))
    REXLOG_INFO("[narutorise] Audio: the game's interface sound {:08X} {}", key,
                played ? "plays in our menus" : "was refused by the game's sound manager");
}

void PlayPending(PPCContext& ctx, uint8_t* base) {
  std::array<bool, kKeys.size()> due{};
  {
    std::lock_guard lock(mutex);
    const auto now = Clock::now();
    for (size_t i = 0; i < pending.size(); ++i) {
      due[i] = pending[i] && now - *pending[i] < kStale;
      pending[i].reset();
    }
  }
  for (size_t i = 0; i < due.size(); ++i)
    if (due[i]) Request(ctx, base, i);
}
}  // namespace

void Play(Sound sound) {
  std::lock_guard lock(mutex);
  auto& when = pending[size_t(sound)];
  // A request left over from before the game ran gives way to this one.
  if (!when || Clock::now() - *when >= kStale) when = Clock::now();
}
}  // namespace audio::ui_sounds

// (manager, 0): the sound manager's update. From the main step, once a tick,
// the menus' requests play first.
REX_HOOK_RAW(sub_82668A78) {
  if (uint32_t(ctx.lr) == audio::ui_sounds::kMainStepReturn) audio::ui_sounds::PlayPending(ctx, base);
  __imp__sub_82668A78(ctx, base);
}
