#include "widescreen/layout.h"

#include <algorithm>
#include <cmath>

namespace widescreen {
namespace {
uint32_t RoundTo16(double value) { return uint32_t(std::lround(value / 16.0)) * 16; }
// The widest logical side whose surface fits at `scale`, a multiple of 16.
uint32_t MaxLogicalSide(float scale) {
  if (!(scale > 1.0f)) return kMaxSurfaceSide;
  return uint32_t(double(kMaxSurfaceSide) / double(scale)) / 16 * 16;
}
}  // namespace

Size LogicalSize(AspectMode mode, uint32_t width, uint32_t height, float scale_x, float scale_y) {
  if (mode == AspectMode::kStandard || !width || !height) return kStandardSize;
  const double aspect = std::clamp(double(width) / double(height), kMinAspect, kMaxAspect);
  // 1280 x 720 itself fits at every scale the renderer admits (up to 4x).
  if (aspect >= 16.0 / 9.0) return {std::clamp(RoundTo16(720.0 * aspect), 1280u, std::max(1280u, MaxLogicalSide(scale_x))), 720};
  return {1280, std::clamp(RoundTo16(1280.0 / aspect), 720u, std::max(720u, MaxLogicalSide(scale_y)))};
}

uint8_t MirrorAnchors(uint8_t anchors) {
  const uint8_t sides = anchors & (kAnchorLeft | kAnchorRight);
  return uint8_t((anchors & ~(kAnchorLeft | kAnchorRight)) | (sides & kAnchorLeft ? kAnchorRight : 0) |
                 (sides & kAnchorRight ? kAnchorLeft : 0));
}

uint32_t EdramTiles(uint32_t width, uint32_t height, uint32_t msaa, bool wide_format) {
  const uint32_t w = msaa == 2 ? width * 2 : width, h = msaa >= 1 ? height * 2 : height;
  return (w + 79) / 80 * 80 * ((h + 15) & ~15u) * (wide_format ? 8 : 4) / 5120;
}

uint32_t EdramBase(uint32_t before, uint32_t size) {
  if (before + size <= kEdramTiles) return before;
  return size < kEdramTiles ? kEdramTiles - size : 0;
}

uint32_t MainDepthEdramBase(Size s) {
  const uint32_t tiles = EdramTiles(s.width, s.height, 0);  // Color and depth: 4 bytes a sample alike.
  return EdramBase(tiles, tiles);
}

uint32_t HalfDepthEdramBase(Size s, uint32_t color_tiles) {
  return EdramBase(color_tiles, EdramTiles(s.width / 2, s.height / 2, 2));
}

std::array<std::array<uint32_t, 4>, 3> TileRects(Size s) {
  const uint32_t row = (s.height / 3 + 31) / 32 * 32;
  return {{{0, 0, s.width, row}, {0, row, s.width, 2 * row}, {0, 2 * row, s.width, s.height}}};
}

float VertPlusFov(float fov, float ratio) {
  constexpr float kBase = 9.0f / 16.0f;
  if (!(ratio > kBase) || !std::isfinite(fov)) return fov;
  return 2.0f * std::atan(std::tan(fov * 0.5f) * ratio / kBase);
}

Viewport CropViewport(float source_w, float source_h, float target_w, float target_h) {
  if (source_w <= 0 || source_h <= 0 || target_w <= 0 || target_h <= 0) return {0, 0, target_w, target_h};
  const float source = source_w / source_h, target = target_w / target_h;
  if (source >= target) {
    const float width = target_h * source;
    return {(target_w - width) * 0.5f, 0, width, target_h};
  }
  const float height = target_w / source;
  return {0, (target_h - height) * 0.5f, target_w, height};
}

Rect MovieRect(Size s, Rect in, uint8_t anchors) {
  const float width = float(s.width), height = float(s.height);
  Rect out{in.x, in.y, in.w * 1280.0f / width, in.h * 720.0f / height};
  if (in.x != 0.0f || in.y != 0.0f) {
    // Placed by a script: its offset is in screen units already (a marker
    // puts the stage's middle on a character, x = its place - 0.5); the stage
    // moves with the 16:9 box, as a full-screen movie does.
    out.x += BoxLeft(s) / width;
    out.y += BoxTop(s) / height;
    return out;
  }
  if (anchors & kAnchorCover) {
    const float scale = std::max(width / 1280.0f, height / 720.0f);
    out.w *= scale;
    out.h *= scale;
    out.x = (1.0f - out.w) * 0.5f;
    out.y = (1.0f - out.h) * 0.5f;
    return out;
  }
  if (anchors & kAnchorFill) return in;  // The whole screen: a movie of flat fills only.
  const float left = BoxLeft(s) / width, top = BoxTop(s) / height;
  if (anchors & kAnchorExtend) out.w = in.w;  // The full width: a movie of flat fills only.
  out.x = anchors & (kAnchorLeft | kAnchorExtend) ? 0.0f : anchors & kAnchorRight ? 2.0f * left : left;
  out.y = anchors & kAnchorTop ? 0.0f : anchors & kAnchorBottom ? 2.0f * top : top;
  return out;
}

Matrix2D AnchorPiece(Size s, Matrix2D m, uint8_t anchors) {
  constexpr float kTwips = 20.0f, kCenterX = 640.0f * kTwips, kCenterY = 360.0f * kTwips;
  const float width = float(s.width), height = float(s.height);
  const auto scale_x = [&](float k) {
    m.a *= k;
    m.c *= k;
    m.tx = m.tx * k + kCenterX * (1.0f - k);
  };
  const auto scale_y = [&](float k) {
    m.b *= k;
    m.d *= k;
    m.ty = m.ty * k + kCenterY * (1.0f - k);
  };
  if (anchors & kAnchorCover) {
    const float k = std::max(width / 1280.0f, height / 720.0f);
    scale_x(k);
    scale_y(k);
    return m;
  }
  if (anchors & (kAnchorExtend | kAnchorFill)) scale_x(width / 1280.0f);
  if (anchors & kAnchorFill) scale_y(height / 720.0f);
  if (anchors & kAnchorLeft) m.tx -= BoxLeft(s) * kTwips;
  if (anchors & kAnchorRight) m.tx += BoxLeft(s) * kTwips;
  if (anchors & kAnchorTop) m.ty -= BoxTop(s) * kTwips;
  if (anchors & kAnchorBottom) m.ty += BoxTop(s) * kTwips;
  return m;
}
}  // namespace widescreen
