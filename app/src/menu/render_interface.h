// The RmlUi render interface over rex::ui::ImmediateDrawer: the native UI layer
// (gpu/ui/immediate_drawer.h) or the SDK's drawer of the Xenos presenter. The
// premultiplied colors and generated textures of RmlUi become the straight
// alpha of the drawer, a transform is applied to the vertices on the CPU, and
// the scissor region is the drawer's. Untextured geometry samples a white
// texture of its own, never a null one: the SDK's D3D12 drawer reads a null
// SRV for it, which comes back 0 (transparent) on some drivers. No clip mask,
// layer, filter or shader:
// the menus are drawn with triangles, textures and scissor rects only. Images
// are decoded files or private procedural/local-game sources: "game/<key>" are
// the in-game menu's (game_assets.h kMenuImageKeys), "generated/<name>" the
// shapes the game's movies draw as vectors. One thread, as RmlUi.
#pragma once

#include <memory>
#include <optional>
#include <RmlUi/Core/RenderInterface.h>
#include <rex/ui/immediate_drawer.h>
#include "menu/game_logo.h"

namespace narutorise::menu {

class RenderInterface final : public Rml::RenderInterface {
 public:
  explicit RenderInterface(rex::ui::ImmediateDrawer& drawer) : drawer_(drawer) {}
  // Around the Render of the RmlUi contexts: the drawer's Begin and End, in a
  // coordinate space of the contexts' size in pixels.
  void Begin(rex::ui::UIDrawContext& context, int width, int height);
  void End();
  // UI thread only. Invalidates the RmlUi source before replacing its pixels.
  void SetGameLogo(std::optional<game_logo::Image> image);
  // UI thread only: the kMenuImages of the game, all of them or none.
  void SetGameArt(std::optional<std::vector<game_assets::Image>> images);
  bool HasGameArt() const { return game_art_.has_value(); }

  Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                              Rml::Span<const int> indices) override;
  void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                      Rml::TextureHandle texture) override;
  void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
  Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override;
  Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) override;
  void ReleaseTexture(Rml::TextureHandle texture) override;
  void EnableScissorRegion(bool enable) override;
  void SetScissorRegion(Rml::Rectanglei region) override;
  void SetTransform(const Rml::Matrix4f* transform) override;

 private:
  Rml::TextureHandle CreateImage(uint32_t width, uint32_t height, const uint8_t* rgba);
  rex::ui::ImmediateDrawer& drawer_;
  std::unique_ptr<rex::ui::ImmediateTexture> white_;  // Created at the first untextured draw.
  bool drawing_ = false;
  bool scissor_ = false;
  Rml::Rectanglei scissor_region_;
  std::optional<Rml::Matrix4f> transform_;
  std::optional<game_logo::Image> game_logo_;
  std::optional<std::vector<game_assets::Image>> game_art_;
};

}  // namespace narutorise::menu
