// Write watch of guest physical memory (RegisterPhysicalMemoryInvalida-
// tionCallback and EnablePhysicalMemoryAccessCallbacks of the SDK, the same the
// SDK's own Xenos backend uses in shared_memory.cpp). A watched page is
// read-only in the three physical views; the first write by any path (title
// code, inline writers, the Bink, a host copy into guest memory) reaches the
// callback inside the global critical region, which unprotects it. The
// callback only counts (no lock: it runs inside that region), per 4 KB page and
// per 1 MB chunk, and returns the notified range, so the heap unwatches only
// those pages and the other textures keep their watch. A read takes the
// counts before arming its pages and reads after: a write after the count is
// either seen by the read or counted for the next bind. Reads go through the
// physical membase, which no watch protects.
#include "gpu/textures/write_watch.h"
#include "gpu/common/guest_memory.h"
#include "gpu/diagnostics/profile.h"
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

namespace gpu::write_watch {
std::atomic<uint32_t> page_writes[kPhysical >> kPageShift];
std::atomic<uint32_t> chunk_writes[kPhysical >> kChunkShift];
std::atomic<uint64_t> epoch{1}, notifications{0}, notified_pages{0}, unlock_ranges{0}, armed_ranges{0};
void Bump(uint32_t physical, uint32_t length) {
  if (!length || physical >= kPhysical) return;
  const uint32_t last = uint32_t(std::min<uint64_t>(uint64_t(physical) + length - 1, kPhysical - 1));
  for (uint32_t p = physical >> kPageShift; p <= last >> kPageShift; ++p) page_writes[p].fetch_add(1, std::memory_order_relaxed);
  for (uint32_t c = physical >> kChunkShift; c <= last >> kChunkShift; ++c) chunk_writes[c].fetch_add(1, std::memory_order_relaxed);
  epoch.fetch_add(1, std::memory_order_release);
}
std::pair<uint32_t, uint32_t> Notified(void*, uint32_t physical, uint32_t length, bool) {
  Bump(physical, length);
  notifications.fetch_add(1, std::memory_order_relaxed);
  notified_pages.fetch_add((uint64_t(length) + 4095) >> kPageShift, std::memory_order_relaxed);
  profile::Count(profile::kWatchNotifications, 1);
  profile::Count(profile::kWatchPages, (uint64_t(length) + 4095) >> kPageShift);
  return {physical, length};
}
uint64_t Sum(const std::vector<std::pair<uint32_t, uint32_t>>& ranges, bool pages) {
  const uint32_t shift = pages ? kPageShift : kChunkShift;
  const std::atomic<uint32_t>* counts = pages ? page_writes : chunk_writes;
  uint64_t sum = 0;
  for (const auto& [at, n] : ranges)
    for (uint32_t i = at >> shift; i <= (at + n - 1) >> shift; ++i) sum += counts[i].load(std::memory_order_relaxed);
  return sum;
}
void Arm(const std::vector<std::pair<uint32_t, uint32_t>>& ranges) {
  // Armed from the translator too, which has no guest context: the memory
  // object attached at the engine creation (the translator has no ThreadState;
  // without it nothing would be armed and rewritten buffers and textures would
  // stay stale).
  auto* memory = GuestMemory();
  if (!memory) return;
  static std::once_flag registered;
  std::call_once(registered, [&] { memory->RegisterPhysicalMemoryInvalidationCallback(Notified, nullptr); });
  for (const auto& [at, n] : ranges) memory->EnablePhysicalMemoryAccessCallbacks(at, n, true, false);
  armed_ranges.fetch_add(ranges.size(), std::memory_order_relaxed);
}
}  // namespace gpu::write_watch
