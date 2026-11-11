// The widescreen mode's options, window size and logical screen
// (widescreen.h), and the sizes of the game's own targets at their call sites.
#include "widescreen/widescreen.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include "gpu/common/guest_memory.h"
#include "gpu/d3d/d3d.h"
#include "gpu/settings.h"

REXCVAR_DEFINE_STRING(aspect_ratio, "auto", "narutorise/Graphics",
                      "Screen aspect of the native renderer: auto follows the window (the monitor in fullscreen), "
                      "from 4:3 to 32:9; 16:9 keeps the game's own screen")
    .allowed({"auto", "16:9"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(hud_world, "edges", "narutorise/Graphics",
                      "Open-world HUD on screens wider than 16:9: edges, or center (the 16:9 middle)")
    .allowed({"edges", "center"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(hud_fight, "center", "narutorise/Graphics",
                      "Fight HUD on screens wider than 16:9: center (the 16:9 middle), or edges")
    .allowed({"edges", "center"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace widescreen {
namespace {
// The options' copies: written by InitializeOptions and the cvars' change
// callbacks, read by the game threads.
std::atomic<AspectMode> aspect_mode{AspectMode::kAuto};
std::atomic<HudPlacement> world_hud{HudPlacement::kEdges};
std::atomic<HudPlacement> fight_hud{HudPlacement::kCenter};

std::atomic<uint64_t> window_size{0};
std::atomic<int64_t> window_changed_at{0};  // steady_clock milliseconds.
constexpr int64_t kWindowSettleMs = 250;
std::atomic<uint64_t> current_size{PackSize(kStandardSize)};
std::atomic<uint64_t> layout_size{PackSize(kStandardSize)};
std::atomic<bool> session_started{false};
std::atomic<uint64_t> delivered_lists{0};
constexpr uint32_t kEffectAspect = 0x8208E228;  // 16/9 of the effect 8211C278 (.rdata).
constexpr uint32_t kPoolTable = 0x8207D410;     // 31 entries of 32 B: index, base, scale, w, h, format...
constexpr uint32_t kThumbnailEntry = 17;        // 0.25 S, R5G6B5: the save thumbnail (320 x 180).

// Call sites in 820E6B00, the creation of the game's targets (the LR of each call).
constexpr uint32_t kFrontBufferLr = 0x820E6BFC;  // +504, CreateTexture W x H.
constexpr uint32_t kHalfDepthLr = 0x820E6CB0;    // +524, depth 2X at W/2 x H/2.
constexpr uint32_t kMainColorLr = 0x820E6CDC;    // +516, color W x H.
constexpr uint32_t kMainDepthLr = 0x820E6D04;    // +520, depth W x H after +516 in EDRAM.
}  // namespace

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void InitializeOptions() {
  static std::once_flag once;
  std::call_once(once, [] {
    // The callbacks run inside the cvar registry's lock, with the new value:
    // they parse it and touch nothing of the registry.
    rex::cvar::RegisterChangeCallback("aspect_ratio", [](std::string_view, std::string_view value) {
      aspect_mode.store(ParseAspectMode(value), std::memory_order_relaxed);
    });
    rex::cvar::RegisterChangeCallback("hud_world", [](std::string_view, std::string_view value) {
      world_hud.store(ParseHudPlacement(value, HudPlacement::kEdges), std::memory_order_relaxed);
    });
    rex::cvar::RegisterChangeCallback("hud_fight", [](std::string_view, std::string_view value) {
      fight_hud.store(ParseHudPlacement(value, HudPlacement::kCenter), std::memory_order_relaxed);
    });
    // A live change of the resolution scale waits until the game's targets
    // fit at it and no change of the screen is under way.
    gpu::SetSettingsGate([](const gpu::Settings& next) {
      const Size s = Current();
      return float(s.width) * next.resolution_scale.x <= float(kMaxSurfaceSide) &&
             float(s.height) * next.resolution_scale.y <= float(kMaxSurfaceSide) && ChangeSettled();
    });
  });
  // The values already loaded; the UI thread is the one that writes them.
  aspect_mode.store(ParseAspectMode(REXCVAR_GET(aspect_ratio)), std::memory_order_relaxed);
  world_hud.store(ParseHudPlacement(REXCVAR_GET(hud_world), HudPlacement::kEdges), std::memory_order_relaxed);
  fight_hud.store(ParseHudPlacement(REXCVAR_GET(hud_fight), HudPlacement::kCenter), std::memory_order_relaxed);
}
AspectMode ConfiguredAspect() { return aspect_mode.load(std::memory_order_relaxed); }
HudPlacement WorldHud() { return world_hud.load(std::memory_order_relaxed); }
HudPlacement FightHud() { return fight_hud.load(std::memory_order_relaxed); }

void SetWindowSize(uint32_t width, uint32_t height) {
  if (!width || !height) return;  // Minimized: the screen stays.
  const uint64_t next = uint64_t(width) << 32 | height;
  if (window_size.exchange(next, std::memory_order_relaxed) == next) return;
  window_changed_at.store(NowMs(), std::memory_order_relaxed);
  // Every size a drag passes; the change itself is logged once (live.cpp).
  REXLOG_DEBUG("[narutorise] widescreen window {}x{}", width, height);
}
bool WindowSettled() { return NowMs() - window_changed_at.load(std::memory_order_relaxed) >= kWindowSettleMs; }
Size Requested() {
  const uint64_t window = window_size.load(std::memory_order_relaxed);
  // The larger of the scale in effect and the one asked for: a larger scale
  // reaches the engine (Reconfigure) only once the targets fit at it, so the
  // screen narrows first (the settings gate, InitializeOptions); a smaller
  // one widens the screen only once it is in effect.
  const gpu::ResolutionScale current = gpu::CurrentResolutionScale(), requested = gpu::RequestedResolutionScale();
  return LogicalSize(ConfiguredAspect(), uint32_t(window >> 32), uint32_t(window), std::max(current.x, requested.x),
                     std::max(current.y, requested.y));
}
Size Current() { return UnpackSize(current_size.load(std::memory_order_acquire)); }
Size Layout() { return UnpackSize(layout_size.load(std::memory_order_acquire)); }
void SetCurrent(Size s) { current_size.store(PackSize(s), std::memory_order_release); }
void SetLayout(Size s) { layout_size.store(PackSize(s), std::memory_order_release); }
void StartSession(Size s) {
  SetCurrent(s);
  SetLayout(s);
  session_started.store(true, std::memory_order_release);
}
bool SessionStarted() { return session_started.load(std::memory_order_acquire); }
bool Enabled() { return gpu::Active() && !Current().Standard(); }
bool LayoutEnabled() { return gpu::Active() && !Layout().Standard(); }

void NoteDelivery() { delivered_lists.fetch_add(1, std::memory_order_acq_rel); }
uint64_t DrawnList() { return delivered_lists.load(std::memory_order_acquire); }
uint64_t BuildingList() { return delivered_lists.load(std::memory_order_acquire) + 1; }

namespace {
// Writes into the game's read-only data (.rdata of default.xex): the host
// page opens for writing and closes again. The guest code reads the host
// memory directly, so only its protection matters; the heap's page table no
// longer counts the image's pages as committed once the session runs, and
// its Protect refuses them then.
bool PatchReadOnly(uint32_t address, const uint8_t* bytes, uint32_t size) {
  auto* memory = gpu::GuestMemory();
  if (!memory) return false;
  uint8_t* host = memory->TranslateVirtual<uint8_t*>(address);
  rex::memory::PageAccess old{};
  if (!rex::memory::Protect(host, size, rex::memory::PageAccess::kReadWrite, &old)) return false;
  std::memcpy(host, bytes, size);
  rex::memory::Protect(host, size, old, nullptr);
  return std::memcmp(host, bytes, size) == 0;
}
void PutBig(uint8_t* bytes, uint32_t value) {
  for (int i = 0; i < 4; ++i) bytes[i] = uint8_t(value >> (24 - 8 * i));
}
uint32_t FloatBits(float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  return v;
}
}  // namespace

bool PatchScreenData(uint8_t*, Size s) {
  // The thumbnail entry: scale 0 takes the entry's own size, 320 x 180 (0.25 of
  // 1280 x 720, the game's own at 16:9), whatever S is.
  uint8_t aspect[4], entry[12];
  PutBig(aspect, FloatBits(float(s.width) / float(s.height)));
  PutBig(entry, FloatBits(0.0f));
  PutBig(entry + 4, 320);
  PutBig(entry + 8, 180);
  return PatchReadOnly(kEffectAspect, aspect, 4) &&
         PatchReadOnly(kPoolTable + 32 * kThumbnailEntry + 8, entry, sizeof(entry));
}

void WriteCamera(uint8_t* base, Size s) {
  const uint32_t gdi = gpu::Get32(base, kGdi);
  if (!gdi) return;
  gpu::Put32(base, gdi + 244, s.width);
  gpu::Put32(base, gdi + 248, s.height);
  gpu::Put32(base, gdi + 280, s.Standard() ? 3 : 0);
  gpu::PutFloat(base, gdi + 260, s.Standard() ? 1.0f : float(s.height) / float(s.width));
}

void AdjustTextureCreate(PPCContext& ctx, uint8_t*) {
  if (ctx.lr != kFrontBufferLr || !Enabled()) return;
  const Size s = Current();
  ctx.r3.u64 = s.width;
  ctx.r4.u64 = s.height;
  REXLOG_INFO("[narutorise] widescreen target +504 (front buffer): {}x{}", s.width, s.height);
}

void AdjustSurfaceCreate(PPCContext& ctx, uint8_t* base) {
  const uint32_t lr = uint32_t(ctx.lr);
  if ((lr != kHalfDepthLr && lr != kMainColorLr && lr != kMainDepthLr) || !Enabled()) return;
  const Size s = Current();
  if (lr == kHalfDepthLr) {  // Its EDRAM base came from 820E76A0 (game_hooks.cpp).
    ctx.r3.u64 = s.width / 2;
    ctx.r4.u64 = s.height / 2;
    REXLOG_INFO("[narutorise] widescreen target +524 (half depth): {}x{}, EDRAM base {}", s.width / 2, s.height / 2,
                ctx.r7.u32 ? gpu::Get32(base, ctx.r7.u32) : 0);
    return;
  }
  ctx.r3.u64 = s.width;
  ctx.r4.u64 = s.height;
  uint32_t edram = 0;
  if (lr == kMainDepthLr && ctx.r7.u32) {  // The li 720 of 820E6CF4 at 1280 x 720.
    edram = MainDepthEdramBase(s);
    gpu::Put32(base, ctx.r7.u32, edram);
  }
  REXLOG_INFO("[narutorise] widescreen target {}: {}x{}, EDRAM base {}", lr == kMainDepthLr ? "+520 (depth)" : "+516 (color)",
              s.width, s.height, edram);
}

std::optional<std::array<std::array<uint32_t, 4>, 3>> TilingRects() {
  if (!Enabled()) return std::nullopt;
  return TileRects(Current());
}
}  // namespace widescreen
