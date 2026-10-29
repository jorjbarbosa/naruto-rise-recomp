// Guard census of the D3D boundary (census.h).
#include "gpu/diagnostics/census.h"
#include "gpu/diagnostics/diagnostics.h"
#include "gpu/d3d/draw.h"
#include "gpu/d3d/frame.h"
#include "gpu/d3d/hooks.h"
#include "gpu/host/services.h"
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

using namespace gpu;

namespace gpu {
int GuardCensus() {
  static const int value = [] {
    if (!Diagnostics()) return 0;
    const char* text = std::getenv("REX_NATIVE_GUARD_CENSUS");
    if (!text || !*text) return 0;
    return std::string_view(text) == "1" ? 1 : -1;
  }();
  return value;
}
bool SkipUncoveredDraws() {
  static const bool value = GuardCensus() > 0 || !Diagnostics();
  return value;
}
struct CensusReason {
  uint64_t count = 0, first_swap = 0, first_own_draw = 0;
};
struct {
  std::mutex mutex;
  std::map<std::string, CensusReason> reasons;
  uint64_t skipped = 0, overflow = 0;
  std::map<std::string, CensusReason> operations;  // Outside the draw.
  uint64_t operations_skipped = 0;
} census;
std::atomic<uint64_t> census_first_skip_swap{0};  // The Swap closing the frame of the first skip; 0: none.
// The occurrence of this reason (from 1), or 0 past the reason cap.
uint64_t NoteCensus(const std::string& reason) {
  const uint64_t swap = swaps.load() + 1;
  uint64_t expected = 0;
  census_first_skip_swap.compare_exchange_strong(expected, swap);
  std::lock_guard lock(census.mutex);
  ++census.skipped;
  auto it = census.reasons.find(reason);
  if (it == census.reasons.end()) {
    if (census.reasons.size() >= kCensusReasons) {
      ++census.overflow;
      return 0;
    }
    it = census.reasons.emplace(reason, CensusReason{0, swap, own_draws.load()}).first;
  }
  return ++it->second.count;
}
bool NoteUncoveredOperation(const std::string& reason, uint32_t lr) {
  if (!SkipUncoveredDraws()) return false;
  const uint64_t swap = swaps.load() + 1;
  uint64_t occurrence = 0;
  {
    std::lock_guard lock(census.mutex);
    ++census.operations_skipped;
    auto it = census.operations.find(reason);
    if (it == census.operations.end()) {
      if (census.operations.size() >= kCensusReasons) {
        ++census.overflow;
        return true;
      }
      it = census.operations.emplace(reason, CensusReason{0, swap, own_draws.load()}).first;
    }
    occurrence = ++it->second.count;
  }
  if (occurrence <= kCensusRecords)
    host::Record("native-d3d-operation-skip", fmt::format(
        "\"reason\":\"{}\",\"occurrence\":{},\"swap\":{},\"own_draws\":{},\"lr\":{}", reason, occurrence, swap,
        own_draws.load(), lr));
  return true;
}
void UncoveredOperation(PPCContext& ctx, const std::string& reason) {
  if (!NoteUncoveredOperation(reason, ctx.lr)) StopInOrder(ctx, reason, 86);
}
// Front-buffer readback of the partial image every kCensusReadEvery Swaps
// after the first skip, kCensusReads times.
bool CensusFrontRead(uint64_t n) {
  const uint64_t first = census_first_skip_swap.load();
  return GuardCensus() > 0 && first && n > first && (n - first) % kCensusReadEvery == 0 &&
         (n - first) / kCensusReadEvery <= kCensusReads;
}
std::string GuardCensusSummary() {
  std::lock_guard lock(census.mutex);
  auto list = [](const std::map<std::string, CensusReason>& from) {
    std::string out;
    for (const auto& [reason, r] : from)
      out += fmt::format("{}{{\"reason\":\"{}\",\"count\":{},\"first_swap\":{},\"first_own_draw\":{}}}",
                         out.empty() ? "" : ",", reason, r.count, r.first_swap, r.first_own_draw);
    return out;
  };
  return fmt::format("\"skipped_draws\":{},\"distinct\":{},\"overflow\":{},\"first_skip_swap\":{},\"swaps\":{},"
                     "\"reasons\":[{}],\"skipped_operations\":{},\"operations\":[{}]", census.skipped,
                     census.reasons.size(), census.overflow, census_first_skip_swap.load(), swaps.load(),
                     list(census.reasons), census.operations_skipped, list(census.operations));
}
}  // namespace gpu
