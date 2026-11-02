// The game's button icons drawn as player one's device (button_icons.h).
#include "gpu/textures/button_icons.h"
#include "controls/glyphs.h"
#include "controls/service.h"
#include "menu/assets.h"
#include <rex/logging.h>
#include <rex/ui/image_decode.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gpu::button_icons {
namespace {
struct Icon {
  uint64_t hash;
  uint32_t width, height;
  const char* input;  // controls::GlyphSprite's input; "-off" for the greyed one
};
constexpr Icon kIcons[] = {
#include "gpu/textures/button_icons.inc"
};
struct Cell {
  const char* name;
  int x, y;
};
constexpr Cell kCells[] = {
#include "controls/glyph_atlas.inc"
};
constexpr int kCell = 128;

uint64_t Fnv64(const std::vector<uint8_t>& bytes) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (const uint8_t b : bytes) h = (h ^ b) * 0x100000001B3ull;
  return h;
}

// The atlas (app/ui/images/prompts.png), decoded once.
struct Atlas {
  int width = 0, height = 0;
  std::vector<uint8_t> rgba;
  std::unordered_map<std::string_view, const Cell*> cells;
};
const Atlas& GlyphAtlas() {
  static Atlas atlas;
  static std::once_flag once;
  std::call_once(once, [] {
    const auto png = narutorise::menu::Asset("images/prompts.png");
    atlas.rgba = rex::ui::DecodeImageRGBA(png.data(), png.size(), atlas.width, atlas.height);
    if (atlas.rgba.empty()) REXLOG_ERROR("[narutorise] The button glyph atlas did not decode");
    for (const auto& cell : kCells) atlas.cells.emplace(cell.name, &cell);
  });
  return atlas;
}

// The dark rim behind every glyph: the game's buttons have one, and Kenney's
// white glyphs (the mouse, the D-pad, the keys) vanish over a bright sky
// without it. It also fills the thin cuts that reach a glyph's edge.
constexpr uint8_t kRim[3] = {20, 20, 24};

// A glyph fit into width x height: its opaque part, scaled by area to keep its
// aspect, centered, over its rim, straight alpha. Transparent texels take a
// neighbour's colour so filtering does not darken the edges.
bool FitGlyph(std::string_view sprite, uint32_t width, uint32_t height, bool off, std::vector<uint8_t>& out) {
  const Atlas& atlas = GlyphAtlas();
  const auto found = atlas.cells.find(sprite);
  if (atlas.rgba.empty() || found == atlas.cells.end()) return false;
  const Cell& cell = *found->second;
  const auto texel = [&](int x, int y) { return &atlas.rgba[(size_t(cell.y + y) * atlas.width + cell.x + x) * 4]; };
  int x0 = kCell, y0 = kCell, x1 = -1, y1 = -1;
  for (int y = 0; y < kCell; ++y)
    for (int x = 0; x < kCell; ++x)
      if (texel(x, y)[3] > 16) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); }
  if (x1 < 0) return false;
  const double cw = x1 - x0 + 1, ch = y1 - y0 + 1;
  const int rim = std::max(1, int(std::lround(std::min(width, height) / 18.0)));
  const double margin = std::max(1.0, std::round(std::min(width, height) * .04)) + rim;
  const double scale = std::min((width - 2 * margin) / cw, (height - 2 * margin) / ch);
  if (scale <= 0) return false;
  const double ox = (width - cw * scale) / 2, oy = (height - ch * scale) / 2;
  out.assign(size_t(width) * height * 4, 0);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      // The source rectangle of the texel, area-weighted (premultiplied).
      const double sx0 = (x - ox) / scale + x0, sx1 = (x + 1 - ox) / scale + x0;
      const double sy0 = (y - oy) / scale + y0, sy1 = (y + 1 - oy) / scale + y0;
      double sum[4] = {}, area = (sx1 - sx0) * (sy1 - sy0);
      for (int sy = int(std::floor(sy0)); sy < int(std::ceil(sy1)); ++sy) {
        if (sy < y0 || sy > y1) continue;
        const double wy = std::min(sy1, sy + 1.0) - std::max(sy0, double(sy));
        for (int sx = int(std::floor(sx0)); sx < int(std::ceil(sx1)); ++sx) {
          if (sx < x0 || sx > x1) continue;
          const double w = wy * (std::min(sx1, sx + 1.0) - std::max(sx0, double(sx)));
          const uint8_t* p = texel(sx, sy);
          const double a = p[3] / 255.0;
          for (int c = 0; c < 3; ++c) sum[c] += w * a * p[c];
          sum[3] += w * a;
        }
      }
      uint8_t* q = &out[(size_t(y) * width + x) * 4];
      const double alpha = area > 0 ? sum[3] / area : 0;
      if (alpha <= 0) continue;
      for (int c = 0; c < 3; ++c) q[c] = uint8_t(std::clamp(sum[c] / sum[3], 0.0, 255.0));
      q[3] = uint8_t(std::clamp(alpha * 255 + .5, 0.0, 255.0));
      if (off) {  // The game's greyed button: no colour, dimmer.
        const double grey = (q[0] * .3 + q[1] * .59 + q[2] * .11) * .65;
        q[0] = q[1] = q[2] = uint8_t(grey);
      }
    }
  }
  {
    // The rim: the glyph's alpha spread over a disc of radius rim, under it.
    std::vector<uint8_t> glyph = out;
    for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width; ++x) {
        double spread = 0;
        for (int dy = -rim - 1; dy <= rim + 1; ++dy)
          for (int dx = -rim - 1; dx <= rim + 1; ++dx) {
            const int nx = int(x) + dx, ny = int(y) + dy;
            if (nx < 0 || ny < 0 || nx >= int(width) || ny >= int(height)) continue;
            const double weight = std::clamp(rim + 1 - std::sqrt(double(dx * dx + dy * dy)), 0.0, 1.0);
            spread = std::max(spread, weight * glyph[(size_t(ny) * width + nx) * 4 + 3] / 255.0);
          }
        uint8_t* q = &out[(size_t(y) * width + x) * 4];
        const double a = q[3] / 255.0, under = spread * (1 - a), total = a + under;
        if (total <= 0) continue;
        for (int c = 0; c < 3; ++c) q[c] = uint8_t(std::clamp((q[c] * a + kRim[c] * under) / total, 0.0, 255.0));
        q[3] = uint8_t(std::clamp(total * 255 + .5, 0.0, 255.0));
      }
  }
  for (int pass = 0; pass < 2; ++pass) {
    std::vector<uint8_t> next = out;
    for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width; ++x) {
        uint8_t* q = &next[(size_t(y) * width + x) * 4];
        if (out[(size_t(y) * width + x) * 4 + 3]) continue;
        for (const auto [dx, dy] : {std::pair{-1, 0}, {1, 0}, {0, -1}, {0, 1}}) {
          const int nx = int(x) + dx, ny = int(y) + dy;
          if (nx < 0 || ny < 0 || nx >= int(width) || ny >= int(height)) continue;
          const uint8_t* n = &out[(size_t(ny) * width + nx) * 4];
          if (n[3] || (n[0] | n[1] | n[2])) { q[0] = n[0]; q[1] = n[1]; q[2] = n[2]; break; }
        }
      }
    out = std::move(next);
  }
  return true;
}

uint16_t To565(const uint8_t* c) {
  return uint16_t(((c[0] * 31 + 127) / 255) << 11 | ((c[1] * 63 + 127) / 255) << 5 | ((c[2] * 31 + 127) / 255));
}
std::array<int, 3> From565(uint16_t c) {
  return {(c >> 11) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31};
}
// One BC3 block of 4x4 straight-alpha texels: the alpha in its eight-step
// mode, the colour on the diagonal of the visible texels' box.
void EncodeBlock(const uint8_t (&px)[16][4], uint8_t* out) {
  uint8_t amin = 255, amax = 0;
  for (const auto& p : px) { amin = std::min(amin, p[3]); amax = std::max(amax, p[3]); }
  out[0] = amax;
  out[1] = amin;
  uint64_t alpha_bits = 0;
  if (amax > amin) {
    std::array<int, 8> palette{amax, amin};
    for (int i = 1; i < 7; ++i) palette[i + 1] = ((7 - i) * amax + i * amin) / 7;
    for (int i = 0; i < 16; ++i) {
      int best = 0;
      for (int k = 1; k < 8; ++k)
        if (std::abs(palette[k] - px[i][3]) < std::abs(palette[best] - px[i][3])) best = k;
      alpha_bits |= uint64_t(best) << (3 * i);
    }
  }
  for (int i = 0; i < 6; ++i) out[2 + i] = uint8_t(alpha_bits >> (8 * i));
  uint8_t lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
  bool any = false;
  for (const auto& p : px) {
    if (!p[3]) continue;
    any = true;
    for (int c = 0; c < 3; ++c) { lo[c] = std::min(lo[c], p[c]); hi[c] = std::max(hi[c], p[c]); }
  }
  if (!any) { std::fill(lo, lo + 3, 0); std::fill(hi, hi + 3, 0); }
  uint16_t c0 = To565(hi), c1 = To565(lo);
  if (c0 < c1) std::swap(c0, c1);
  const auto e0 = From565(c0), e1 = From565(c1);
  std::array<std::array<int, 3>, 4> palette{e0, e1};
  for (int c = 0; c < 3; ++c) {
    palette[2][c] = (2 * e0[c] + e1[c]) / 3;
    palette[3][c] = (e0[c] + 2 * e1[c]) / 3;
  }
  uint32_t color_bits = 0;
  for (int i = 0; i < 16; ++i) {
    int best = 0, best_distance = 1 << 30;
    for (int k = 0; k < 4; ++k) {
      int distance = 0;
      for (int c = 0; c < 3; ++c) distance += (palette[k][c] - px[i][c]) * (palette[k][c] - px[i][c]);
      if (distance < best_distance) { best = k; best_distance = distance; }
    }
    color_bits |= uint32_t(best) << (2 * i);
  }
  out[8] = uint8_t(c0); out[9] = uint8_t(c0 >> 8);
  out[10] = uint8_t(c1); out[11] = uint8_t(c1 >> 8);
  for (int i = 0; i < 4; ++i) out[12 + i] = uint8_t(color_bits >> (8 * i));
}
std::vector<uint8_t> EncodeBC3(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) {
  const uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
  std::vector<uint8_t> out(size_t(bw) * bh * 16);
  for (uint32_t by = 0; by < bh; ++by)
    for (uint32_t bx = 0; bx < bw; ++bx) {
      uint8_t px[16][4];
      for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t x = std::min(bx * 4 + i % 4, width - 1), y = std::min(by * 4 + i / 4, height - 1);
        std::copy_n(&rgba[(size_t(y) * width + x) * 4], 4, px[i]);
      }
      EncodeBlock(px, &out[(size_t(by) * bw + bx) * 16]);
    }
  return out;
}
}  // namespace

int32_t Identify(const DrawTexture& t) {
  if (t.format != TextureFormat::kBC3Unorm || t.cube || t.data.empty() || t.width > 128 || t.height > 128) return -1;
  const uint64_t hash = Fnv64(t.data[0]);
  for (size_t i = 0; i < std::size(kIcons); ++i)
    if (kIcons[i].hash == hash && kIcons[i].width == t.width && kIcons[i].height == t.height) {
      static std::array<std::atomic<bool>, std::size(kIcons)> seen{};
      if (!seen[i].exchange(true))
        REXLOG_INFO("[narutorise] Button icon {} ({}x{}) drawn as the player's device", kIcons[i].input, t.width, t.height);
      return int32_t(i);
    }
  return -1;
}

bool Replace(int32_t icon, DrawTexture& t) {
  if (icon < 0 || size_t(icon) >= std::size(kIcons) || t.data.size() < t.levels) return false;
  std::string_view input = kIcons[icon].input;
  const bool off = input.ends_with("-off");
  if (off) input.remove_suffix(4);
  const auto config = narutorise::controls::Current();
  const auto sprite = narutorise::controls::GlyphSprite(input, narutorise::controls::PublishedGlyphs(), config->ports[0]);
  if (sprite.empty()) return false;
  std::vector<std::vector<uint8_t>> levels;
  std::vector<uint8_t> rgba;
  for (uint32_t level = 0; level < t.levels; ++level) {
    const uint32_t w = std::max(t.width >> level, 1u), h = std::max(t.height >> level, 1u);
    if (!FitGlyph(sprite, w, h, off, rgba)) return false;
    auto blocks = EncodeBC3(rgba, w, h);
    if (blocks.size() != t.data[level].size()) return false;
    levels.push_back(std::move(blocks));
  }
  for (uint32_t level = 0; level < t.levels; ++level) t.data[level] = std::move(levels[level]);
  return true;
}

uint64_t Revision() { return narutorise::controls::GlyphsRevision(); }
}  // namespace gpu::button_icons
