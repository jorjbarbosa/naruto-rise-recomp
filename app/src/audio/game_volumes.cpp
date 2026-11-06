// The game's own volumes (game_volumes.h).
#include "audio/game_volumes.h"
#include "game_options.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <mutex>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>

extern "C" void __imp__sub_825A55F0(PPCContext& __restrict, uint8_t*);

namespace audio::game_volumes {
namespace {
using Clock = std::chrono::steady_clock;
using narutorise::game_options::Session;
constexpr uint32_t kSave = 0x825BC508;  // (options, count): the options into the player's profile.
constexpr auto kSaveAfter = std::chrono::seconds(1);
constexpr auto kSaveRetry = std::chrono::seconds(2);
// Each volume's option in the game's array, and the groups fn_Option_Apply sets for it.
struct Target {
  uint32_t option;
  std::array<uint32_t, 4> groups;
  uint32_t count;
  const char* name;
};
constexpr Target kVolumes[kCount] = {{0, {0}, 1, "music"}, {1, {2}, 1, "voice"}, {2, {1, 3, 4, 5}, 4, "effects"}};

std::atomic<int> pending[kCount] = {-1, -1, -1};
std::mutex save_mutex;
std::optional<Clock::time_point> save_due;
bool save_failed = false;  // The last save failed (logged once until one succeeds).

uint32_t Get32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, 4);
  return std::byteswap(value);
}
void Put32(uint8_t* base, uint32_t address, uint32_t value) {
  value = std::byteswap(value);
  std::memcpy(base + address, &value, 4);
}

// The options' array the script modules reported, while the game's memory
// holds it whole and writable, with its length first.
uint32_t Array(rex::memory::Memory* memory) {
  const uint32_t array = narutorise::game_options::Shared().Options();
  if (!array) return 0;
  auto* heap = memory->LookupHeap(array);
  const uint32_t last = array + 4 * Session::kArrayLength + 3;
  const auto wanted = uint32_t(rex::memory::PageAccess::kReadWrite);
  if (!heap || (uint32_t(heap->QueryCommittedRangeAccess(array, last)) & wanted) != wanted) return 0;
  return Get32(memory->virtual_membase(), array) == Session::kArrayLength ? array : 0;
}
uint32_t Slot(uint32_t array, Volume volume) { return array + 4 + 4 * kVolumes[volume].option; }
}  // namespace

std::optional<std::array<int, kCount>> Read() {
  auto* kernel = rex::system::kernel_state();
  if (!kernel) return std::nullopt;
  const uint32_t array = Array(kernel->memory());
  if (!array) return std::nullopt;
  const uint8_t* base = kernel->memory()->virtual_membase();
  std::array<int, kCount> values{};
  for (int volume = 0; volume < kCount; ++volume)
    values[size_t(volume)] = std::clamp(int(Get32(base, Slot(array, Volume(volume)))), 0, kMaximum);
  return values;
}

bool Set(Volume volume, int value) {
  auto* kernel = rex::system::kernel_state();
  const uint32_t array = kernel ? Array(kernel->memory()) : 0;
  if (!array) return false;
  value = std::clamp(value, 0, kMaximum);
  Put32(kernel->memory()->virtual_membase(), Slot(array, volume), uint32_t(value));
  pending[volume] = value;
  return true;
}

void ApplyPending(PPCContext& ctx, uint8_t* base) {
  for (int volume = 0; volume < kCount; ++volume) {
    const int value = pending[volume].exchange(-1);
    if (value < 0) continue;
    // As fn_Option_Apply: the level from 0 to 1 into each group of the volume.
    std::array<uint8_t, sizeof(PPCContext)> saved;
    std::memcpy(saved.data(), &ctx, sizeof(PPCContext));
    for (uint32_t i = 0; i < kVolumes[volume].count; ++i) {
      ctx.r3.u64 = kVolumes[volume].groups[i];
      ctx.f1.f64 = double(std::clamp(float(value) * 0.1f, 0.0f, 1.0f));
      __imp__sub_825A55F0(ctx, base);
      std::memcpy(&ctx, saved.data(), sizeof(PPCContext));
    }
    REXLOG_INFO("[narutorise] Audio: the game's {} volume set to {}", kVolumes[volume].name, value);
    std::lock_guard lock(save_mutex);
    save_due = Clock::now() + kSaveAfter;
  }
}

void SaveIfDue() {
  {
    std::lock_guard lock(save_mutex);
    if (!save_due || Clock::now() < *save_due) return;
    save_due.reset();
  }
  auto* kernel = rex::system::kernel_state();
  const uint32_t array = Array(kernel->memory());
  if (!array) return;
  uint64_t args[] = {array + 4, Session::kArrayLength};
  const bool saved = kernel->function_dispatcher()->Execute(rex::runtime::ThreadState::Get(), kSave, args, 2) != 0;
  std::lock_guard lock(save_mutex);
  if (!saved && !save_due) save_due = Clock::now() + kSaveRetry;
  if (saved || !save_failed)
    REXLOG_INFO("[narutorise] Audio: the game's options {}", saved ? "saved into the player's profile"
                                                                   : "wait for a player to save them");
  save_failed = !saved;
}
}  // namespace audio::game_volumes
