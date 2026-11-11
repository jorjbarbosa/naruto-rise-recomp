#pragma once

// The logical screen of the widescreen mode and the arithmetic that follows
// from it. The game renders at W x H: H = 720 above 16:9 (Hor+), W = 1280
// below it (Vert+), both multiples of 16, between 4:3 and 32:9. 1280 x 720 is
// the game as it ships, and every user of this header leaves the game alone
// there. The Flash stage keeps its 1280 x 720 pixels inside the screen: one
// logical pixel is one stage pixel on both axes, so nothing is stretched.
// Plain data, no SDK: the unit tests and the menu take it as it is.
#include <array>
#include <cstdint>
#include <string_view>

namespace widescreen {
enum class AspectMode : uint8_t { kAuto, kStandard };  // aspect_ratio: "auto", "16:9"
enum class HudPlacement : uint8_t { kEdges, kCenter };  // hud_world / hud_fight: "edges", "center"

// The options' values as narutorise.toml spells them; anything else is the
// option's default (`fallback` for the HUD).
inline AspectMode ParseAspectMode(std::string_view value) {
  return value == "16:9" ? AspectMode::kStandard : AspectMode::kAuto;
}
inline HudPlacement ParseHudPlacement(std::string_view value, HudPlacement fallback) {
  return value == "center" ? HudPlacement::kCenter : value == "edges" ? HudPlacement::kEdges : fallback;
}

struct Size {
  uint32_t width = 1280, height = 720;
  bool Standard() const { return width == 1280 && height == 720; }
  friend bool operator==(const Size&, const Size&) = default;
};
inline constexpr Size kStandardSize{1280, 720};
inline constexpr double kMinAspect = 4.0 / 3.0, kMaxAspect = 32.0 / 9.0;
// A screen in one word, for the atomics that hand it between threads.
inline constexpr uint64_t PackSize(Size s) { return uint64_t(s.width) << 32 | s.height; }
inline constexpr Size UnpackSize(uint64_t v) { return {uint32_t(v >> 32), uint32_t(v)}; }

// The widest side of a surface the native engine creates (both backends): the
// logical screen times the resolution scale stays within it.
inline constexpr uint32_t kMaxSurfaceSide = 8192;

// The logical screen for a window (or monitor) of width x height. The 16:9
// mode, or a side of zero (a minimized window), gives 1280 x 720. A screen
// whose side, times the resolution scale of that axis, would pass
// kMaxSurfaceSide is narrowed to the widest that fits (2048 x 720 at 4x).
Size LogicalSize(AspectMode mode, uint32_t width, uint32_t height, float scale_x = 1.0f, float scale_y = 1.0f);

// The 16:9 box (the 1280 x 720 stage) centered in the logical screen: the
// distance from the screen's left and top edges to the box, in logical pixels.
inline float BoxLeft(Size s) { return (float(s.width) - 1280.0f) * 0.5f; }
inline float BoxTop(Size s) { return (float(s.height) - 720.0f) * 0.5f; }

// EDRAM tiles of a surface, as the game counts them (820E76A0): msaa 0, 1 or 2
// (1, 2 or 4 samples), 4 bytes a sample, 8 for the wide formats.
uint32_t EdramTiles(uint32_t width, uint32_t height, uint32_t msaa, bool wide_format = false);
inline constexpr uint32_t kEdramTiles = 2048;
// The base of a surface of `size` tiles placed after one of `before` tiles:
// `before`, or lower when the pair would pass the 2048 tiles, which the
// creation (8212D330) refuses. The native renderer keeps no EDRAM; only the
// game's check needs the pair to fit.
uint32_t EdramBase(uint32_t before, uint32_t size);
// The EDRAM bases of the game's targets at a screen (820E6B00), the same at
// the session's start and after a change: the main depth (+520, W x H) after
// the main color (+516, W x H), and the half-resolution depth (+524, W/2 x H/2
// 2X) after `color_tiles`, the tiles 820E76A0 counts for a W/2 x H/2 4X color.
uint32_t MainDepthEdramBase(Size s);
uint32_t HalfDepthEdramBase(Size s, uint32_t color_tiles);

// The three tile rectangles {x1, y1, x2, y2} of the main 3D pass: the
// table at 82081798 for 1280 x 720 ({0,0,1280,256}, {0,256,1280,512},
// {0,512,1280,720}); full width and rows of a 32-aligned third otherwise.
std::array<std::array<uint32_t, 4>, 3> TileRects(Size s);

// The vertical field of view (radians) that keeps the 16:9 horizontal one on
// a screen taller than 16:9 (Vert+); ratio = H / W of the screen. Unchanged at
// 16:9 and wider.
float VertPlusFov(float fov, float ratio);

// The viewport that draws a source of source_w x source_h into a target of
// target_w x target_h at the same scale on both axes, centered, so the target
// shows the middle of the source cropped to its own aspect.
struct Viewport {
  float x = 0, y = 0, width = 0, height = 0;
};
Viewport CropViewport(float source_w, float source_h, float target_w, float target_h);

// Anchors of a Flash movie in the logical screen.
enum Anchor : uint8_t {
  kAnchorNone = 0,
  kAnchorLeft = 1,     // ESQ: the stage's left edge on the screen's left edge.
  kAnchorRight = 2,    // DIR
  kAnchorTop = 4,      // TOPO (Vert+)
  kAnchorBottom = 8,   // BASE (Vert+)
  kAnchorCover = 16,   // COBRIR: the stage scaled alike on both axes until it covers the screen.
  kAnchorExtend = 32,  // ESTENDER: the stage widened to the screen's width; flat fills only (bars).
  kAnchorFill = 64,    // ESTENDER on both axes: a flat fill or fade over the whole screen.
};
// The anchors of a mirrored copy of a movie (the opponent's half of a shared
// HUD): left and right trade places; the rest stays.
uint8_t MirrorAnchors(uint8_t anchors);
// A normalized rectangle of the screen (x, y, width, height), as the game's
// Flash renderer keeps it (renderer+552).
struct Rect {
  float x = 0, y = 0, w = 1, h = 1;
  friend bool operator==(const Rect&, const Rect&) = default;
};
// The rectangle of a movie on the logical screen. The stage keeps its pixel
// size (w and h shrink by 1280/W and 720/H). A rectangle at the origin is a
// full-screen movie: it goes into the 16:9 box, moved to the edges its anchors
// name. Any other rectangle was placed by a script in screen coordinates (a
// marker over a character: x = its place - 0.5, so the stage's middle lands on
// it): the box moves it as well, and anchors do not apply.
Rect MovieRect(Size s, Rect in, uint8_t anchors);

// A GFx 2.x GMatrix2D as the game's renderer reads it (SetMatrix, 825E8490):
// x' = a·x + c·y + tx, y' = b·x + d·y + ty, in twips of the stage.
struct Matrix2D {
  float a = 1, c = 0, tx = 0, b = 0, d = 1, ty = 0;
};
// The transform of a piece of a movie that sits in the 16:9 box, moved to
// the edges its anchors name (left/right/top/bottom), widened to the
// screen's width (extend) or scaled alike on both axes to cover it (cover),
// around the stage's center. In stage twips (20 per pixel).
Matrix2D AnchorPiece(Size s, Matrix2D m, uint8_t anchors);
// A copy of a repeated piece: moved `offset` stage pixels along the piece's
// own x axis (its direction on the stage, mirrored or turned as the piece
// is), whatever the piece's scale. The catalog measures the periods on the
// stage.
Matrix2D RepeatPiece(Matrix2D m, float offset);
}  // namespace widescreen
