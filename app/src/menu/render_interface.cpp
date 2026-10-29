// The RmlUi render interface over rex::ui::ImmediateDrawer (render_interface.h).
#include "menu/render_interface.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
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
