// The RmlUi render interface over rex::ui::ImmediateDrawer (render_interface.h).
#include "menu/render_interface.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Vertex.h>
#include <rex/ui/image_decode.h>

namespace narutorise::menu {
namespace {
// A geometry in chunks the drawer's 16-bit indices reach, its vertices in
// straight alpha.
struct Geometry {
  struct Chunk {
    std::vector<rex::ui::ImmediateVertex> vertices;
    std::vector<uint16_t> indices;
  };
  std::vector<Chunk> chunks;
};
struct Image {
  std::unique_ptr<rex::ui::ImmediateTexture> texture;
};

uint8_t Unpremultiply(uint8_t value, uint8_t alpha) {
  return alpha ? uint8_t(std::min(255, (value * 255 + alpha / 2) / alpha)) : 0;
}
// R8G8B8A8 with red in the low byte, straight alpha.
uint32_t Straight(const Rml::ColourbPremultiplied& c) {
  return uint32_t(Unpremultiply(c.red, c.alpha)) | uint32_t(Unpremultiply(c.green, c.alpha)) << 8 |
         uint32_t(Unpremultiply(c.blue, c.alpha)) << 16 | uint32_t(c.alpha) << 24;
}
rex::ui::ImmediateVertex Converted(const Rml::Vertex& v) {
  return {v.position.x, v.position.y, v.tex_coord.x, v.tex_coord.y, Straight(v.colour)};
}

// The shapes the game's Options movie draws as vectors, rasterized here: the
// renderer has no gradient shader. Layers are composited back to front over
// transparency, with a pixel of edge smoothing on signed distances.
using Color = std::array<float, 3>;
Color Hex(uint32_t rgb) { return {float(rgb >> 16 & 255), float(rgb >> 8 & 255), float(rgb & 255)}; }
float Cover(float distance) { return std::clamp(0.5f - distance, 0.0f, 1.0f); }
float Segment(float px, float py, float ax, float ay, float bx, float by) {
  const float dx = bx - ax, dy = by - ay;
  const float t = std::clamp(((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
  return std::hypot(px - ax - t * dx, py - ay - t * dy);
}
struct Canvas {
  uint32_t width, height;
  std::vector<std::array<float, 4>> pixels;
  Canvas(uint32_t w, uint32_t h) : width(w), height(h), pixels(size_t(w) * h, {0, 0, 0, 0}) {}
  void Over(size_t i, const Color& c, float a) {
    auto& d = pixels[i];
    const float out = a + d[3] * (1 - a);
    if (out > 0) for (int k = 0; k < 3; ++k) d[k] = (c[k] * a + d[k] * d[3] * (1 - a)) / out;
    d[3] = out;
  }
  game_assets::Image Image() const {
    game_assets::Image image{width, height, std::vector<uint8_t>(pixels.size() * 4)};
    for (size_t i = 0; i < pixels.size(); ++i) {
      for (int k = 0; k < 3; ++k) image.rgba[i * 4 + k] = uint8_t(std::lround(std::clamp(pixels[i][k], 0.0f, 255.0f)));
      image.rgba[i * 4 + 3] = uint8_t(std::lround(pixels[i][3] * 255));
    }
    return image;
  }
};
constexpr uint32_t kInk = 0x1E1914;
// The ON/OFF orb: dark rim, white ring, a radial gradient lit from the top left.
game_assets::Image Orb(bool on) {
  const std::array<Color, 3> stops = on ? std::array{Hex(0xFFD27A), Hex(0xF07010), Hex(0xA82400)}
                                        : std::array{Hex(0xFAFAFA), Hex(0x9A9A9A), Hex(0x383838)};
  Canvas canvas(64, 64);
  for (uint32_t y = 0; y < 64; ++y) for (uint32_t x = 0; x < 64; ++x) {
    const size_t i = size_t(y) * 64 + x;
    const float d = std::hypot(x + .5f - 32, y + .5f - 32);
    const float t = std::clamp(std::hypot(x + .5f - 25.6f, y + .5f - 22.4f) / 48, 0.0f, 1.0f);
    const float u = t < .55f ? t / .55f : (t - .55f) / .45f;
    const Color& a = t < .55f ? stops[0] : stops[1];
    const Color& b = t < .55f ? stops[1] : stops[2];
    canvas.Over(i, Hex(kInk), Cover(d - 31));
    canvas.Over(i, Hex(0xFFFFFF), Cover(d - 29));
    canvas.Over(i, {a[0] + (b[0] - a[0]) * u, a[1] + (b[1] - a[1]) * u, a[2] + (b[2] - a[2]) * u}, Cover(d - 26));
  }
  return canvas.Image();
}
// The ‹ › of a choice: a white triangle with an ink border.
game_assets::Image Arrow(bool right) {
  Canvas canvas(40, 56);
  const float ax = 36, ay = 4, bx = 4, by = 28, cx = 36, cy = 52;
  for (uint32_t y = 0; y < 56; ++y) for (uint32_t x = 0; x < 40; ++x) {
    const float px = (right ? 39 - x : x) + .5f, py = y + .5f;
    const float edge = std::min({Segment(px, py, ax, ay, bx, by), Segment(px, py, bx, by, cx, cy),
                                 Segment(px, py, cx, cy, ax, ay)});
    const auto side = [&](float x0, float y0, float x1, float y1) { return (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0); };
    const float s1 = side(ax, ay, bx, by), s2 = side(bx, by, cx, cy), s3 = side(cx, cy, ax, ay);
    const bool inside = (s1 >= 0 && s2 >= 0 && s3 >= 0) || (s1 <= 0 && s2 <= 0 && s3 <= 0);
    const float distance = inside ? -edge : edge;
    const size_t i = size_t(y) * 40 + x;
    canvas.Over(i, Hex(kInk), Cover(distance));
    canvas.Over(i, Hex(0xFFFFFF), Cover(distance + 3));
  }
  return canvas.Image();
}
// The › of an action: a thick white chevron with an ink border.
game_assets::Image Chevron() {
  Canvas canvas(32, 48);
  for (uint32_t y = 0; y < 48; ++y) for (uint32_t x = 0; x < 32; ++x) {
    const float px = x + .5f, py = y + .5f;
    const float d = std::min(Segment(px, py, 8, 6, 24, 24), Segment(px, py, 24, 24, 8, 42));
    const size_t i = size_t(y) * 32 + x;
    canvas.Over(i, Hex(kInk), Cover(d - 6));
    canvas.Over(i, Hex(0xFFFFFF), Cover(d - 3.5f));
  }
  return canvas.Image();
}
std::optional<game_assets::Image> Shape(std::string_view name) {
  if (name == "generated/orb-on") return Orb(true);
  if (name == "generated/orb-off") return Orb(false);
  if (name == "generated/arrow-left") return Arrow(false);
  if (name == "generated/arrow-right") return Arrow(true);
  if (name == "generated/chevron") return Chevron();
  return {};
}
}  // namespace

void RenderInterface::Begin(rex::ui::UIDrawContext& context, int width, int height) {
  drawer_.Begin(context, float(width), float(height));
  drawing_ = true;
}

void RenderInterface::End() {
  drawing_ = false;
  drawer_.End();
}

Rml::CompiledGeometryHandle RenderInterface::CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                             Rml::Span<const int> indices) {
  auto geometry = std::make_unique<Geometry>();
  if (vertices.size() <= 65536) {
    auto& chunk = geometry->chunks.emplace_back();
    chunk.vertices.reserve(vertices.size());
    for (const auto& v : vertices) chunk.vertices.push_back(Converted(v));
    chunk.indices.reserve(indices.size());
    for (int i : indices) chunk.indices.push_back(uint16_t(i));
  } else {  // Whole triangles, each chunk with its own vertices.
    std::vector<int> local(vertices.size(), -1);
    Geometry::Chunk* chunk = nullptr;
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
      if (!chunk || chunk->vertices.size() + 3 > 65536) {
        std::fill(local.begin(), local.end(), -1);
        chunk = &geometry->chunks.emplace_back();
      }
      for (size_t k = 0; k < 3; ++k) {
        const int index = indices[t + k];
        if (local[index] < 0) {
          local[index] = int(chunk->vertices.size());
          chunk->vertices.push_back(Converted(vertices[index]));
        }
        chunk->indices.push_back(uint16_t(local[index]));
      }
    }
  }
  return reinterpret_cast<Rml::CompiledGeometryHandle>(geometry.release());
}

void RenderInterface::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation,
                                     Rml::TextureHandle texture) {
  if (!drawing_ || !handle) return;
  if (!texture && !white_) {
    const uint8_t white[4]{0xFF, 0xFF, 0xFF, 0xFF};
    white_ = drawer_.CreateTexture(1, 1, rex::ui::ImmediateTextureFilter::kNearest, false, white);
  }
  const auto& geometry = *reinterpret_cast<const Geometry*>(handle);
  for (const auto& chunk : geometry.chunks) {
    std::vector<rex::ui::ImmediateVertex> vertices = chunk.vertices;
    for (auto& v : vertices) {
      Rml::Vector2f p{v.x + translation.x, v.y + translation.y};
      if (transform_) {
        const Rml::Vector4f projected = *transform_ * Rml::Vector4f(p.x, p.y, 0, 1);
        const float w = projected.w != 0 ? projected.w : 1;
        p = {projected.x / w, projected.y / w};
      }
      v.x = p.x;
      v.y = p.y;
    }
    drawer_.BeginDrawBatch({vertices.data(), int(vertices.size()), chunk.indices.data(), int(chunk.indices.size())});
    rex::ui::ImmediateDraw draw;
    draw.count = int(chunk.indices.size());
    draw.texture = texture ? reinterpret_cast<Image*>(texture)->texture.get() : white_.get();
    draw.scissor = scissor_;
    draw.scissor_left = float(scissor_region_.Left());
    draw.scissor_top = float(scissor_region_.Top());
    draw.scissor_right = float(scissor_region_.Right());
    draw.scissor_bottom = float(scissor_region_.Bottom());
    drawer_.Draw(draw);
    drawer_.EndDrawBatch();
  }
}

void RenderInterface::ReleaseGeometry(Rml::CompiledGeometryHandle handle) {
  delete reinterpret_cast<Geometry*>(handle);
}

Rml::TextureHandle RenderInterface::CreateImage(uint32_t width, uint32_t height, const uint8_t* rgba) {
  auto texture = drawer_.CreateTexture(width, height, rex::ui::ImmediateTextureFilter::kLinear, false, rgba);
  if (!texture) return 0;
  return reinterpret_cast<Rml::TextureHandle>(new Image{std::move(texture)});
}

Rml::TextureHandle RenderInterface::LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) {
  if (std::string_view(source).starts_with("game/")) {
    if (!game_art_) return 0;
    const auto key = std::string_view(source).substr(5);
    for (size_t i = 0; i < game_assets::kMenuImageKeys.size(); ++i) {
      if (game_assets::kMenuImageKeys[i] != key) continue;
      const auto& image = (*game_art_)[i];
      dimensions = {int(image.width), int(image.height)};
      return CreateImage(image.width, image.height, image.rgba.data());
    }
    return 0;
  }
  if (const auto shape = Shape(source)) {
    dimensions = {int(shape->width), int(shape->height)};
    return CreateImage(shape->width, shape->height, shape->rgba.data());
  }
  if (source == "local/game-logo") {
    if (!game_logo_) return 0;
    dimensions = {int(game_logo_->width), int(game_logo_->height)};
    return CreateImage(game_logo_->width, game_logo_->height, game_logo_->rgba.data());
  }
  if (source == "generated/setup-background") {
    // Original procedural background: no game artwork or raster file embedded.
    constexpr int w = 256, h = 144;
    std::vector<uint8_t> pixels(w * h * 4);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
      const float dx = (float(x) / (w - 1) - .5f) * 1.7f;
      const float dy = (float(y) / (h - 1) - .35f);
      const float light = std::max(0.f, 1.f - std::sqrt(dx * dx + dy * dy) / .65f);
      auto* p = pixels.data() + (y * w + x) * 4;
      p[0] = uint8_t(8 + 16 * light); p[1] = uint8_t(15 + 23 * light);
      p[2] = uint8_t(27 + 32 * light); p[3] = 255;
    }
    dimensions = {w, h};
    return CreateImage(w, h, pixels.data());
  }
  Rml::FileInterface* files = Rml::GetFileInterface();
  const Rml::FileHandle file = files ? files->Open(source) : 0;
  if (!file) return 0;
  std::vector<uint8_t> bytes(files->Length(file));
  const size_t read = files->Read(bytes.data(), bytes.size(), file);
  files->Close(file);
  int width = 0, height = 0;
  const auto rgba = read == bytes.size() ? rex::ui::DecodeImageRGBA(bytes.data(), bytes.size(), width, height)
                                         : std::vector<uint8_t>{};
  if (rgba.empty()) return 0;
  dimensions = {width, height};
  return CreateImage(uint32_t(width), uint32_t(height), rgba.data());  // PNG texels are straight alpha already.
}

void RenderInterface::SetGameLogo(std::optional<game_logo::Image> image) {
  Rml::ReleaseTexture("local/game-logo", this);
  game_logo_ = std::move(image);
}

void RenderInterface::SetGameArt(std::optional<std::vector<game_assets::Image>> images) {
  for (const auto key : game_assets::kMenuImageKeys) Rml::ReleaseTexture("game/" + std::string(key), this);
  if (images && images->size() != game_assets::kMenuImageKeys.size()) images.reset();
  game_art_ = std::move(images);
}

Rml::TextureHandle RenderInterface::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) {
  if (dimensions.x <= 0 || dimensions.y <= 0 || source.size() != size_t(dimensions.x) * dimensions.y * 4) return 0;
  std::vector<uint8_t> rgba(source.begin(), source.end());
  for (size_t i = 0; i < rgba.size(); i += 4)
    for (size_t c = 0; c < 3; ++c) rgba[i + c] = Unpremultiply(rgba[i + c], rgba[i + 3]);
  return CreateImage(uint32_t(dimensions.x), uint32_t(dimensions.y), rgba.data());
}

void RenderInterface::ReleaseTexture(Rml::TextureHandle texture) {
  delete reinterpret_cast<Image*>(texture);
}

void RenderInterface::EnableScissorRegion(bool enable) { scissor_ = enable; }

void RenderInterface::SetScissorRegion(Rml::Rectanglei region) { scissor_region_ = region; }

void RenderInterface::SetTransform(const Rml::Matrix4f* transform) {
  transform_ = transform ? std::optional<Rml::Matrix4f>(*transform) : std::nullopt;
}

}  // namespace narutorise::menu
