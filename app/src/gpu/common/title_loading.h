// What the title is loading, read by the backend without depending on the
// host (header only; the hooks live in host/title_loading.cpp).
//
// TitleLoading is true from the call that shows the loading screen to the
// return of the call that hides it (sub_82685460 and sub_82684C48): the
// equivalent of the loading flag the recomps read to raise the priority of the
// pipeline compilation while the player waits anyway.
//
// WorldKey is the key of the world file (".wow") the title loaded last
// (sub_8264B3B0, the loader its pump calls; the key of "Loading world:
// 0x%08X": the village, an arena, a story scene), 0 before the first. A
// pipeline created while a world is the last one loaded is recorded
// with its key; the list built from those records knows the pipelines of each
// world, and the next load of that world posts them before its first draw.
//
// TitleMenu is true while the world loaded last is the one of the title's
// menus (the last file of the boot's burst): the player picks a mode or a
// save there and no world of the
// game runs, so the precompile pool takes all its threads, as on a loading
// screen, but at the lowest priority.
#pragma once

#include <atomic>
#include <cstdint>

namespace gpu {
inline std::atomic<bool> title_loading{false};
inline std::atomic<uint32_t> title_loadings{0};  // Loading screens shown since the start.
inline std::atomic<uint32_t> world_key{0};
inline std::atomic<uint32_t> world_loads{0};  // Worlds loaded since the start.
inline constexpr uint32_t kTitleMenuWorldKey = 0x002C8924;
inline bool TitleLoading() { return title_loading.load(std::memory_order_relaxed); }
inline uint32_t WorldKey() { return world_key.load(std::memory_order_relaxed); }
inline bool TitleMenu() { return WorldKey() == kTitleMenuWorldKey; }
}  // namespace gpu
