// rex::ui::ImmediateDrawer over the UI layer (layer.h): the SDK's immediate
// drawing (indexed triangles with a texture and a scissor rect, straight
// alpha) for what draws the UI in the native mode, on one thread. A texture
// lives in the layer while its object does; the draws between Begin and End
// become one UiFrame, set at End. Lines are not drawn, and every texture is
// sampled linear and clamped, whatever its filter and repeat (the UI layer of
// the engine has one sampler).
#pragma once

#include "gpu/backend/contracts.h"
#include <rex/ui/immediate_drawer.h>

namespace gpu::ui {
class Layer;

class ImmediateDrawer final : public rex::ui::ImmediateDrawer {
 public:
  explicit ImmediateDrawer(Layer& layer) : layer_(layer) {}
  std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(uint32_t width, uint32_t height,
                                                           rex::ui::ImmediateTextureFilter filter, bool is_repeated,
                                                           const uint8_t* data) override;
  void Begin(rex::ui::UIDrawContext& context, float space_width, float space_height) override;
  void BeginDrawBatch(const rex::ui::ImmediateDrawBatch& batch) override;
  void Draw(const rex::ui::ImmediateDraw& draw) override;
  void EndDrawBatch() override;
  void End() override;

 private:
  Layer& layer_;
  UiFrame frame_;
  // The open batch: where its vertices and indices start in the frame.
  uint32_t batch_vertex_ = 0, batch_index_ = 0;
  bool batch_indexed_ = false;
};

}  // namespace gpu::ui
