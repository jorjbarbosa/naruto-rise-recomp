// The game hooks of the widescreen mode: the logical screen in the
// presentation parameters and the camera (820E6D68), the EDRAM base of the
// half-resolution depth (820E76A0), Vert+ (82584860), the Bink videos in the
// 16:9 box (820EC008), the save thumbnail cropped to 16:9 (8211D9A8 and
// 8212AC00), the Flash movies at their place in the screen (826A42B8,
// 8264F768, 825E8230), the root pieces of the menus (the display traversal
// 821F4CE8, 82206740, 821B9750, 821FDFF8 and SetMatrix 825E8490) and the
// image elements (8264F1A0). With the logical screen at 1280 x 720 every hook
// runs the original alone. Each hook names the registers and the offsets it
// reads; they come from a static study of default.xex (the functions'
// callers, their call sites and the structures they fill).
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <unordered_set>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>

#include "gpu/common/guest_memory.h"
#include "gpu/d3d/d3d.h"
#include "widescreen/catalog.h"
#include "widescreen/widescreen.h"

REXCVAR_DEFINE_BOOL(widescreen_log, false, "narutorise/Graphics",
                    "Logs each Flash movie the native renderer draws, with its rectangle and rule (the catalog of "
                    "the widescreen mode)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

extern "C" void __imp__sub_820E6D68(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_820E76A0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82584860(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_820EC008(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8211D9A8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8212AC00(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_826A42B8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8264F768(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8264F1A0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_825E8230(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_825E8490(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_821F4CE8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82206740(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_821B9750(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_821FDFF8(PPCContext& __restrict, uint8_t*);

using namespace widescreen;
using gpu::Get32;
using gpu::GetFloat;
using gpu::Put32;
using gpu::PutFloat;

namespace {
// --- Flash ---
// The movie being drawn on this thread: set by the element's Draw, read by
// BeginDisplay inside it.
struct Movie {
  std::string name;
  MovieRule rule;
  uint32_t element = 0;
};
thread_local const Movie* current_movie = nullptr;
std::atomic<int64_t> last_fight_draw{0};  // steady_clock milliseconds.

// The slot of a small table kept by recency (its `at`, 0 = empty): the one
// `match` names, else an empty one, else the oldest.
template <typename Slot, size_t N, typename Match>
Slot& SlotFor(std::array<Slot, N>& table, Match match) {
  Slot* empty = nullptr;
  for (auto& slot : table) {
    if (slot.at && match(slot)) return slot;
    if (!empty && !slot.at) empty = &slot;
  }
  if (empty) return *empty;
  return *std::min_element(table.begin(), table.end(), [](const Slot& a, const Slot& b) { return a.at < b.at; });
}

// A shared HUD movie drawn by two elements in the same frame is a fight: the
// player's bar and the opponent's (HUD53). Other shared pieces (the context
// buttons) draw once and do not count: the elements are kept per movie.
struct SharedDraw {
  uint16_t movie = 0;  // MovieRule::id.
  uint32_t element = 0;
  int64_t at = 0;
};
std::mutex shared_mutex;
std::array<SharedDraw, 8> shared_draws{};
constexpr int64_t kFightHoldMs = 1500;
// True when another element drew the same shared movie within 100 ms.
bool SecondSharedElement(const Movie& movie, int64_t now, uint32_t* other) {
  const uint16_t key = movie.rule.id;
  std::lock_guard lock(shared_mutex);
  SharedDraw& slot = SlotFor(shared_draws, [key](const SharedDraw& draw) { return draw.movie == key; });
  const bool second =
      slot.at && slot.movie == key && slot.element && slot.element != movie.element && now - slot.at < 100;
  *other = slot.element;
  slot = {key, movie.element, now};
  return second;
}
// The side an element's movie faces: a piece that repeats toward its own -x
// (Repeat::kBefore, the outer side of the player's bar) draws mirrored (a < 0)
// in the opponent's copy of the shared HUD. Learned at the piece's SetMatrix,
// used by the next BeginDisplay of the element: the opponent's copy takes the
// other edge of the screen.
struct ElementSide {
  uint32_t element = 0;
  bool mirrored = false;
  int64_t at = 0;
};
std::mutex side_mutex;
std::array<ElementSide, 8> element_sides{};
void NoteElementSide(uint32_t element, bool mirrored, int64_t now) {
  std::lock_guard lock(side_mutex);
  SlotFor(element_sides, [element](const ElementSide& side) { return side.element == element; }) = {element, mirrored, now};
}
bool ElementMirrored(uint32_t element) {
  if (!element) return false;
  std::lock_guard lock(side_mutex);
  for (const auto& side : element_sides)
    if (side.at && side.element == element) return side.mirrored;
  return false;
}

// Floats of guest memory replaced for one call of the original: read when
// the scope starts, written back when it ends.
template <size_t N>
class GuestFloats {
 public:
  GuestFloats(uint8_t* base, uint32_t address) : base_(base), address_(address) {
    for (size_t i = 0; i < N; ++i) saved_[i] = GetFloat(base, address + 4 * uint32_t(i));
  }
  ~GuestFloats() { Write(saved_); }
  GuestFloats(const GuestFloats&) = delete;
  GuestFloats& operator=(const GuestFloats&) = delete;
  const std::array<float, N>& saved() const { return saved_; }
  void Write(const std::array<float, N>& values) {
    for (size_t i = 0; i < N; ++i) PutFloat(base_, address_ + 4 * uint32_t(i), values[i]);
  }

 private:
  uint8_t* base_;
  uint32_t address_;
  std::array<float, N> saved_{};
};
// The display traversal of a movie with pieces in the catalog: the sprite
// level (0 outside the movie's root, 1 inside _level0) and the depths of the
// pieces being displayed from the root down (more than kMaxPath keep counting,
// unrecorded), and the copy of a repeated piece being drawn (its shift along
// its own x, in its pixels).
struct PiecePath {
  std::array<uint16_t, ClipRule::kMaxPath> depths{};
  int length = 0;
};
thread_local int flash_level = 0;
thread_local PiecePath flash_path;
thread_local float repeat_offset = 0;
const ClipRule* CurrentPiece(const MovieRule& rule) {
  return FindPiece(rule, flash_path.depths.data(), size_t(std::min<int>(flash_path.length, ClipRule::kMaxPath)));
}
void EnterPiece(uint32_t depth) {
  if (flash_path.length < int(ClipRule::kMaxPath)) flash_path.depths[flash_path.length] = uint16_t(depth);
  ++flash_path.length;
}

// The element's name (elem+100: char*, elem+104 bit 3: UTF-16), ASCII only,
// read through the heap's page table (a stale pointer reads nothing).
std::string ElementName(uint8_t* base, uint32_t elem) {
  const uint32_t text = Get32(base, elem + 100);
  if (!text) return {};
  const bool wide = Get32(base, elem + 104) & 8;
  auto* memory = gpu::GuestMemory();
  uint8_t bytes[256];
  uint32_t size = 0;
  // The string may end near the end of its page, before one that is not
  // committed: then up to the end of its page.
  const uint32_t page_rest = 0x1000 - (text & 0xFFF);
  for (uint32_t want : {256u, std::min(256u, page_rest)})
    if (gpu::CopyFromGuest(memory, text, bytes, want)) {
      size = want;
      break;
    }
  std::string out;
  for (uint32_t i = 0; i + (wide ? 1 : 0) < size; i += wide ? 2 : 1) {
    const uint32_t c = wide ? uint32_t(bytes[i]) << 8 | bytes[i + 1] : bytes[i];
    if (!c) break;
    out += c < 0x80 ? char(c) : '?';
  }
  return out;
}

struct MovieScope {
  Movie movie;
  const Movie* previous;
  int previous_level;
  PiecePath previous_path;
  float previous_offset;
  MovieScope(std::string name)
      : previous(current_movie), previous_level(flash_level), previous_path(flash_path),
        previous_offset(repeat_offset) {
    movie.rule = FindMovieRule(name);
    movie.name = std::move(name);
    current_movie = &movie;
    flash_level = 0;
    flash_path = {};
    repeat_offset = 0;
  }
  ~MovieScope() {
    current_movie = previous;
    flash_level = previous_level;
    flash_path = previous_path;
    repeat_offset = previous_offset;
  }
};
bool TracksPieces() { return current_movie && current_movie->rule.clip_count; }

// A piece with a rule, once per movie and path (the root pieces of the menus
// and the deeper pieces the catalog names).
void LogPiece(const Movie& movie, const ClipRule* clip) {
  const int length = std::min<int>(flash_path.length, ClipRule::kMaxPath);
  if (length > 1 && !clip) return;
  std::string path;
  for (int i = 0; i < length; ++i) path += fmt::format("/d{}", flash_path.depths[i]);
  static std::mutex mutex;
  static std::unordered_set<std::string> seen;
  const std::string key = movie.name + "|" + path;
  {
    std::lock_guard lock(mutex);
    if (seen.size() >= 2000 || !seen.insert(key).second) return;
  }
  REXLOG_INFO("[narutorise] widescreen piece \"{}\" {}: anchors {}, repeat {}", movie.name, path,
              clip ? clip->anchors : 0, clip ? clip->repeat : 0);
}

void LogMovie(const Movie* movie, const Rect& in, const Rect& out, uint8_t anchors) {
  static std::mutex mutex;
  static std::unordered_set<std::string> seen;
  const std::string name = movie ? movie->name : std::string("(global)");
  // A movie at the origin by its rectangle; one a script places (a marker
  // that follows a character moves every frame) once.
  const bool placed = in.x != 0.0f || in.y != 0.0f;
  const std::string key =
      placed ? name + "|placed" : fmt::format("{}|{:.3f},{:.3f},{:.3f},{:.3f}", name, in.x, in.y, in.w, in.h);
  {
    std::lock_guard lock(mutex);
    if (seen.size() >= 2000 || !seen.insert(key).second) return;
  }
  REXLOG_INFO("[narutorise] widescreen movie \"{}\": rect [{:.4f},{:.4f},{:.4f},{:.4f}] -> [{:.4f},{:.4f},{:.4f},{:.4f}], "
              "catalog {}, anchors {}, group {}", name, in.x, in.y, in.w, in.h, out.x, out.y, out.w, out.h,
              movie && movie->rule.known, anchors, movie ? int(movie->rule.group) : 0);
}

// The black frame around a 16:9 picture (a framed movie): the screen outside
// the box, black in the picture the game presents. The 3D scene is composed
// into the color target under the interface at points of the frame that vary
// (a clear before the movie draws loses to it in some frames), so the frame is
// applied to the copy into the front buffer (+504): in the list the movie
// draws in (the one the main thread builds) and the kFrameHoldLists after it,
// every resolve into it clears these rectangles first. Lists, not time: the
// render thread draws the list whenever it gets to it.
constexpr uint64_t kFrameHoldLists = 2;
void RequestFrame(uint8_t* base, Size s) {
  const uint32_t owner = Get32(base, kDeviceOwner);
  const uint32_t front = owner ? Get32(base, owner + 504) : 0;
  if (!front) return;
  const int32_t w = int32_t(s.width), h = int32_t(s.height);
  const int32_t left = int32_t(BoxLeft(s)), top = int32_t(BoxTop(s));
  std::array<std::array<int32_t, 4>, 4> rects{};
  size_t count = 0;
  if (left > 0) {
    rects[count++] = {0, 0, left, h};
    rects[count++] = {left + 1280, 0, w, h};
  }
  if (top > 0) {
    rects[count++] = {0, 0, w, top};
    rects[count++] = {0, top + 720, w, h};
  }
  const uint64_t list = BuildingList();
  if (count)
    gpu::RequestFrontFrame(front, std::span<const std::array<int32_t, 4>>(rects.data(), count), list,
                           list + kFrameHoldLists);
}

// --- Save thumbnail ---
// The second reduction of the capture (pool[18] -> pool[17], LR 8211D90C)
// draws the screen's aspect into the 320 x 180 thumbnail: the viewport that
// D3DDevice_SetViewport (8212AC00) writes is replaced by one that crops the
// middle 16:9 of the source.
struct Crop {
  bool active = false;
  float source_w = 0, source_h = 0;
  uint32_t target_w = 0, target_h = 0;  // Only the viewport of the destination crops.
};
thread_local Crop crop;
}  // namespace

// The presentation parameters, the screen of the camera and the copies of
// the size (820E6D68, owner in r3), before the device and its targets exist.
REX_HOOK_RAW(sub_820E6D68) {
  const uint32_t owner = ctx.r3.u32;
  __imp__sub_820E6D68(ctx, base);
  if (!gpu::Active()) return;
  const Size s = Requested();
  StartSession(s);
  if (s.Standard()) {
    REXLOG_INFO("[narutorise] widescreen: 1280x720 (aspect_ratio = {})",
                ConfiguredAspect() == AspectMode::kStandard ? "16:9" : "auto");
    return;
  }
  // The copies that become S (820E81B0): the post-process pool, the viewport
  // and the Bink quad follow S. The back buffer of the presentation parameters
  // (+324/+328) stays 1280 x 720: only CreateDevice reads it (the runtime's own
  // front buffers, unused since the game presents +504), and the native
  // device admits that profile alone (video_services.h).
  Put32(base, owner + 528, s.width);
  Put32(base, owner + 532, s.height);
  WriteCamera(base, s);  // Hor+ by the free ratio.
  // The effect with a fixed 16/9 (8211C278), and the thumbnail entry of the
  // pool fixed at 320 x 180.
  const bool patched = PatchScreenData(base, s);
  REXLOG_INFO("[narutorise] widescreen: {}x{} (aspect_ratio = auto), HUD world {}, fight {}, data {}", s.width,
              s.height, WorldHud() == HudPlacement::kCenter ? "center" : "edges",
              FightHud() == HudPlacement::kCenter ? "center" : "edges", patched ? "patched" : "NOT patched");
}

// EDRAM tiles of a surface; at LR 820E6C8C the base of the half-resolution
// depth +524: the tiles of a W/2 x H/2 4X color, kept below the 2048 tiles
// together with the depth itself (2560 x 720 would pass them).
REX_HOOK_RAW(sub_820E76A0) {
  if (ctx.lr != 0x820E6C8C || !Enabled()) {
    __imp__sub_820E76A0(ctx, base);
    return;
  }
  const Size s = Current();
  ctx.r3.u64 = s.width / 2;
  ctx.r4.u64 = s.height / 2;
  __imp__sub_820E76A0(ctx, base);
  ctx.r3.u64 = HalfDepthEdramBase(s, ctx.r3.u32);
}

// Viewport and focal lengths of a camera (r3 = ScreenFormat, r4 = camera).
// Taller than 16:9, a perspective camera gets the vertical field of view
// that keeps the 16:9 horizontal one, for this call only.
REX_HOOK_RAW(sub_82584860) {
  const uint32_t format = ctx.r3.u32, camera = ctx.r4.u32;
  bool widened = false;
  float fov = 0;
  if (LayoutEnabled() && camera && (Get32(base, camera) & 1) && Get32(base, format + 0x1C) == 0) {
    const float ratio = GetFloat(base, format + 8);
    if (ratio > 0.5625f + 1e-4f) {
      fov = GetFloat(base, camera + 0xC);
      PutFloat(base, camera + 0xC, VertPlusFov(fov, ratio));
      widened = true;
    }
  }
  __imp__sub_82584860(ctx, base);
  if (widened) PutFloat(base, camera + 0xC, fov);
}

// The Bink quad (820EC008, r3 = the video's texture set: +0 HBINK, +4/+8 the
// destination in pixels of S). The vertices are computed here, into a list
// the render thread draws later, so the video moves by its destination: the
// 1280 x 720 videos sit in the 16:9 box at their own pixel size, never
// stretched; the rest of the screen keeps the frame's clear. The box is the
// one of Current(), not Layout(): the game converts the destination by S,
// which changes with the targets (live.cpp, step B), so the two stay
// consistent in the one list built between steps A and B.
REX_HOOK_RAW(sub_820EC008) {
  const uint32_t texset = ctx.r3.u32;
  if (!Enabled() || !texset) {
    __imp__sub_820EC008(ctx, base);
    return;
  }
  const Size s = Current();
  const uint32_t x = Get32(base, texset + 4), y = Get32(base, texset + 8);
  Put32(base, texset + 4, x + uint32_t(BoxLeft(s)));
  Put32(base, texset + 8, y + uint32_t(BoxTop(s)));
  static std::atomic<uint32_t> records{0};
  if (const uint32_t bink = Get32(base, texset); bink && records.fetch_add(1) < 4)  // HBINK: +0 width, +4 height.
    REXLOG_INFO("[narutorise] widescreen Bink {}x{} at {},{}", Get32(base, bink), Get32(base, bink + 4),
                x + uint32_t(BoxLeft(s)), y + uint32_t(BoxTop(s)));
  __imp__sub_820EC008(ctx, base);
  Put32(base, texset + 4, x);
  Put32(base, texset + 8, y);
}

// A reduction of the post-process (8211D9A8: r4 = destination entry, r5 =
// source entry; +12/+16 = width/height). The thumbnail's (LR 8211D90C) crops.
REX_HOOK_RAW(sub_8211D9A8) {
  if (ctx.lr != 0x8211D90C || !Enabled() || !ctx.r5.u32) {
    __imp__sub_8211D9A8(ctx, base);
    return;
  }
  crop = {true, float(Get32(base, ctx.r5.u32 + 12)), float(Get32(base, ctx.r5.u32 + 16)),
          Get32(base, ctx.r4.u32 + 12), Get32(base, ctx.r4.u32 + 16)};
  __imp__sub_8211D9A8(ctx, base);
  crop.active = false;
}

// D3DDevice_SetViewport (r3 = device, r4 = D3DVIEWPORT9 {X, Y, Width,
// Height, MinZ, MaxZ}). The original clamps the viewport to the target; under
// the thumbnail's crop, the viewport of the destination (0, 0, its size) gets
// its transform (PA_CL_VPORT_X/YSCALE and OFFSET, the device's
// +10504..+10516) written again unclamped. The pass's closing viewport (the
// screen's) stays.
REX_HOOK_RAW(sub_8212AC00) {
  const uint32_t device = ctx.r3.u32, request = ctx.r4.u32;
  if (!crop.active || Get32(base, request) || Get32(base, request + 4) || Get32(base, request + 8) != crop.target_w ||
      Get32(base, request + 12) != crop.target_h) {
    __imp__sub_8212AC00(ctx, base);
    return;
  }
  const float target_w = float(crop.target_w), target_h = float(crop.target_h);
  __imp__sub_8212AC00(ctx, base);
  const Viewport v = CropViewport(crop.source_w, crop.source_h, target_w, target_h);
  PutFloat(base, device + 10504, v.width * 0.5f);
  PutFloat(base, device + 10508, v.width * 0.5f + v.x);
  PutFloat(base, device + 10512, -v.height * 0.5f);
  PutFloat(base, device + 10516, v.height * 0.5f + v.y);
}

// The Draw of a movie element by name (type 1, vtable 0x82084958, r3 = element).
REX_HOOK_RAW(sub_826A42B8) {
  if (!gpu::Active() || (!LayoutEnabled() && !REXCVAR_GET(widescreen_log))) {
    __imp__sub_826A42B8(ctx, base);
    return;
  }
  MovieScope scope(ElementName(base, ctx.r3.u32));
  scope.movie.element = ctx.r3.u32;
  __imp__sub_826A42B8(ctx, base);
}

// The Draw of the global movie element (type 4, without a name).
REX_HOOK_RAW(sub_8264F768) {
  if (!gpu::Active() || (!LayoutEnabled() && !REXCVAR_GET(widescreen_log))) {
    __imp__sub_8264F768(ctx, base);
    return;
  }
  MovieScope scope("(global)");
  __imp__sub_8264F768(ctx, base);
}

// GRenderer::BeginDisplay of the game (r3 = renderer): the viewport matrix
// comes from the movie's normalized rectangle at renderer+552..+564, which
// BeginDisplay alone reads. The rectangle of the logical screen for the call.
REX_HOOK_RAW(sub_825E8230) {
  const bool enabled = LayoutEnabled();
  if (!enabled && !REXCVAR_GET(widescreen_log)) {
    __imp__sub_825E8230(ctx, base);
    return;
  }
  const uint32_t rect_address = ctx.r3.u32 + 552;  // The renderer's normalized rectangle.
  const Rect in{GetFloat(base, rect_address), GetFloat(base, rect_address + 4), GetFloat(base, rect_address + 8),
                GetFloat(base, rect_address + 12)};
  const Movie* movie = current_movie;
  const MovieRule rule = movie ? movie->rule : MovieRule{};
  const int64_t now = NowMs();
  if (rule.group == Group::kFight) last_fight_draw.store(now, std::memory_order_relaxed);
  if (rule.group == Group::kShared && movie) {
    uint32_t previous = 0;
    if (SecondSharedElement(*movie, now, &previous)) {
      last_fight_draw.store(now, std::memory_order_relaxed);
      static std::atomic<bool> recorded{false};
      if (!recorded.exchange(true))
        REXLOG_INFO("[narutorise] widescreen: two \"{}\" elements ({:08X}, {:08X}): a fight", movie->name, previous,
                    movie->element);
    }
  }
  const bool fight_on_screen = now - last_fight_draw.load(std::memory_order_relaxed) < kFightHoldMs;
  uint8_t anchors = EffectiveAnchors(rule, WorldHud(), FightHud(), fight_on_screen);
  if (rule.clip_count && ElementMirrored(movie->element)) anchors = MirrorAnchors(anchors);  // The opponent's copy.
  const Rect out = enabled ? MovieRect(Layout(), in, anchors) : in;
  if (REXCVAR_GET(widescreen_log)) LogMovie(movie, in, out, anchors);
  if (!enabled) {
    __imp__sub_825E8230(ctx, base);
    return;
  }
  if (rule.framed) RequestFrame(base, Layout());
  GuestFloats<4> rect(base, rect_address);
  rect.Write({out.x, out.y, out.w, out.h});
  __imp__sub_825E8230(ctx, base);
}

// --- Pieces of the menus ---
// GFxSprite::Display (vtable 0x82008BD8 slot +76, r3 = sprite; depth at +24):
// a child of _level0 is a root piece, its children the next depths of the
// path. Only for movies with pieces in the catalog.
REX_HOOK_RAW(sub_821F4CE8) {
  if (!TracksPieces()) {
    __imp__sub_821F4CE8(ctx, base);
    return;
  }
  const bool piece = flash_level >= 1;
  if (piece) EnterPiece(Get32(base, ctx.r3.u32 + 24));
  ++flash_level;
  __imp__sub_821F4CE8(ctx, base);
  --flash_level;
  if (piece) --flash_path.length;
}

namespace {
// The Display of a character without children (r3 = character, depth at
// +24): the last depth of its path (the borders of the pause menu are shapes
// placed on _level0). A piece that repeats displays again for each copy, from
// the same registers, with the copy's shift for SetMatrix.
void DisplayLeaf(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext& __restrict, uint8_t*)) {
  if (!TracksPieces() || flash_level < 1) {
    original(ctx, base);
    return;
  }
  EnterPiece(Get32(base, ctx.r3.u32 + 24));
  const ClipRule* clip = LayoutEnabled() ? CurrentPiece(current_movie->rule) : nullptr;
  const int copies = clip && clip->repeat_side != Repeat::kNone && repeat_offset == 0.0f
                         ? RepeatCopies(BoxLeft(Layout()), float(clip->repeat))
                         : 0;
  if (!copies) {
    original(ctx, base);
    --flash_path.length;
    return;
  }
  std::array<uint8_t, sizeof(PPCContext)> entry{};
  std::memcpy(entry.data(), &ctx, sizeof(PPCContext));
  original(ctx, base);
  for (const int side : {-1, 1}) {
    if (side > 0 && clip->repeat_side != Repeat::kBoth) continue;
    for (int i = 1; i <= copies; ++i) {
      std::memcpy(&ctx, entry.data(), sizeof(PPCContext));
      repeat_offset = float(side * i * clip->repeat);
      original(ctx, base);
    }
  }
  repeat_offset = 0;
  --flash_path.length;
}
}  // namespace
REX_HOOK_RAW(sub_82206740) { DisplayLeaf(ctx, base, __imp__sub_82206740); }  // Shape (GFxGenericCharacter).
REX_HOOK_RAW(sub_821B9750) { DisplayLeaf(ctx, base, __imp__sub_821B9750); }  // Edit text.
REX_HOOK_RAW(sub_821FDFF8) { DisplayLeaf(ctx, base, __imp__sub_821FDFF8); }  // Button.

// GRenderer::SetMatrix of the game (r3 = renderer, r4 = GMatrix2D in stage
// twips): the matrix of a piece with anchors or of a repeated copy, for the
// call; the original expands it and multiplies by the viewport matrix of
// BeginDisplay. A copy moves along the piece's own x before the anchors.
REX_HOOK_RAW(sub_825E8490) {
  const Movie* movie = current_movie;
  if (!movie || !movie->rule.clip_count || flash_path.length < 1 || !LayoutEnabled()) {
    __imp__sub_825E8490(ctx, base);
    return;
  }
  const ClipRule* clip = CurrentPiece(movie->rule);
  const uint8_t anchors = clip ? clip->anchors : kAnchorNone;
  if (REXCVAR_GET(widescreen_log)) LogPiece(*movie, clip);
  const uint32_t matrix = ctx.r4.u32;
  if (matrix && movie->element && clip && clip->repeat_side == Repeat::kBefore && repeat_offset == 0.0f)
    NoteElementSide(movie->element, GetFloat(base, matrix) < 0.0f, NowMs());  // GMatrix2D a.
  if ((!anchors && repeat_offset == 0.0f) || !matrix) {
    __imp__sub_825E8490(ctx, base);
    return;
  }
  GuestFloats<6> floats(base, matrix);
  const auto& saved = floats.saved();
  Matrix2D piece{saved[0], saved[1], saved[2], saved[3], saved[4], saved[5]};
  constexpr float kTwips = 20.0f;
  piece.tx += piece.a * repeat_offset * kTwips;
  piece.ty += piece.b * repeat_offset * kTwips;
  const Matrix2D m = AnchorPiece(Layout(), piece, anchors);
  floats.Write({m.a, m.c, m.tx, m.b, m.d, m.ty});
  __imp__sub_825E8490(ctx, base);
}

// The Draw of an image element (type 3, vtable 0x82083A40; r3 = element,
// r4 = the parent's normalized rectangle): a textured quad over its rectangle
// (+32) composed with the parent's, in NDC of the whole screen, so wider
// screens stretched it (the save thumbnail of the Load Game menu). The
// parent's rectangle maps into the 16:9 box for the call: the image keeps the
// pixel size and place it has at 1280 x 720, next to the Flash menu around it.
REX_HOOK_RAW(sub_8264F1A0) {
  const uint32_t parent = ctx.r4.u32;
  if (!LayoutEnabled() || !parent) {
    __imp__sub_8264F1A0(ctx, base);
    return;
  }
  const Size s = Layout();
  GuestFloats<4> rect(base, parent);
  const auto& saved = rect.saved();
  const float sx = 1280.0f / float(s.width), sy = 720.0f / float(s.height);
  rect.Write({BoxLeft(s) / float(s.width) + saved[0] * sx, BoxTop(s) / float(s.height) + saved[1] * sy, saved[2] * sx,
              saved[3] * sy});
  if (REXCVAR_GET(widescreen_log)) {
    static std::atomic<uint32_t> records{0};
    if (records.fetch_add(1) < 16)
      REXLOG_INFO("[narutorise] widescreen image element {:08X}: rect [{:.4f},{:.4f},{:.4f},{:.4f}], parent "
                  "[{:.4f},{:.4f},{:.4f},{:.4f}]", ctx.r3.u32, GetFloat(base, ctx.r3.u32 + 32),
                  GetFloat(base, ctx.r3.u32 + 36), GetFloat(base, ctx.r3.u32 + 40), GetFloat(base, ctx.r3.u32 + 44),
                  saved[0], saved[1], saved[2], saved[3]);
  }
  __imp__sub_8264F1A0(ctx, base);
}
