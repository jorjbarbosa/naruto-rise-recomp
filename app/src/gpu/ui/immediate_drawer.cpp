// rex::ui::ImmediateDrawer over the UI layer (immediate_drawer.h).
#include "gpu/ui/immediate_drawer.h"
#include "gpu/ui/layer.h"
#include <cstring>

namespace gpu::ui {
namespace {
// A texture of the layer, released with its object.
class Texture final : public rex::ui::ImmediateTexture {
 public:
  Texture(Layer& layer, uint32_t key, uint32_t width, uint32_t height)
      : ImmediateTexture(width, height), layer_(layer), key_(key) {}
  ~Texture() override { layer_.ReleaseTexture(key_); }
  uint32_t key() const { return key_; }

 private:
  Layer& layer_;
  const uint32_t key_;
};
}  // namespace

std::unique_ptr<rex::ui::ImmediateTexture> ImmediateDrawer::CreateTexture(uint32_t width, uint32_t height,
                                                                          rex::ui::ImmediateTextureFilter,
                                                                          bool, const uint8_t* data) {
  if (!width || !height) return nullptr;
  std::vector<uint8_t> rgba(size_t(width) * height * 4, 0xFF);
  if (data) std::memcpy(rgba.data(), data, rgba.size());
  return std::make_unique<Texture>(layer_, layer_.CreateTexture(width, height, std::move(rgba)), width, height);
}

void ImmediateDrawer::Begin(rex::ui::UIDrawContext& context, float space_width, float space_height) {
  rex::ui::ImmediateDrawer::Begin(context, space_width, space_height);
  frame_ = {};
  frame_.width = coordinate_space_width();
  frame_.height = coordinate_space_height();
}

void ImmediateDrawer::BeginDrawBatch(const rex::ui::ImmediateDrawBatch& batch) {
  batch_vertex_ = uint32_t(frame_.vertices.size());
  batch_index_ = uint32_t(frame_.indices.size());
  batch_indexed_ = batch.indices != nullptr;
  if (batch.vertices && batch.vertex_count > 0) {
    // ImmediateVertex and UiVertex: position, texture coordinates, R8G8B8A8.
    static_assert(sizeof(rex::ui::ImmediateVertex) == sizeof(UiVertex));
    const size_t at = frame_.vertices.size();
    frame_.vertices.resize(at + size_t(batch.vertex_count));
    std::memcpy(&frame_.vertices[at], batch.vertices, size_t(batch.vertex_count) * sizeof(UiVertex));
  }
  if (batch.indices && batch.index_count > 0)
    frame_.indices.insert(frame_.indices.end(), batch.indices, batch.indices + batch.index_count);
}

void ImmediateDrawer::Draw(const rex::ui::ImmediateDraw& draw) {
  if (draw.primitive_type != rex::ui::ImmediatePrimitiveType::kTriangles || draw.count <= 0) return;
  UiDraw d;
  d.texture = draw.texture ? static_cast<const Texture*>(draw.texture)->key() : 0;
  d.vertex_offset = batch_vertex_ + uint32_t(draw.base_vertex);
  d.index_count = uint32_t(draw.count);
  if (batch_indexed_) {
    d.index_offset = batch_index_ + uint32_t(draw.index_offset);
  } else {  // The vertices in order, from base_vertex.
    d.index_offset = uint32_t(frame_.indices.size());
    for (int i = 0; i < draw.count; ++i) frame_.indices.push_back(uint16_t(i));
  }
  d.scissor = draw.scissor;
  d.scissor_left = draw.scissor_left;
  d.scissor_top = draw.scissor_top;
  d.scissor_right = draw.scissor_right;
  d.scissor_bottom = draw.scissor_bottom;
  frame_.draws.push_back(d);
}

void ImmediateDrawer::EndDrawBatch() {}

void ImmediateDrawer::End() {
  layer_.SetFrame(std::move(frame_));
  frame_ = {};
  rex::ui::ImmediateDrawer::End();
}

}  // namespace gpu::ui
