#pragma once

// RmlUi's render interface over the renderer's UI bridge (gpu/ui.h). Host main thread only.
//
// Geometry stays on the CPU: each RenderGeometry appends its vertices, indices and one draw to the
// frame being built, which ui.cpp submits once RmlUi has rendered the context. Textures are bridge
// textures: RmlUi's generated ones (font atlases, premultiplied RGBA8), and images loaded by name,
// "game:<name>" from Naruto_clip.SRA or a DDS file under data/ui.

#include "gpu/ui.h"

#include <RmlUi/Core/RenderInterface.h>

namespace ui
{
    class GameArchive;

    class RenderInterface final : public Rml::RenderInterface
    {
    public:
        // The prefix of a game image's source in the documents.
        static constexpr const char* kGamePrefix = "game:";

        explicit RenderInterface(GameArchive& archive);

        // Starts a frame of this size in pixels.
        void BeginFrame(uint32_t width, uint32_t height);
        // The frame built since BeginFrame.
        gpu::ui::Frame TakeFrame();

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
        GameArchive& archive_;
        gpu::ui::Frame frame_;
        bool scissored_ = false;
        gpu::ui::Scissor scissor_;
        float transform_[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    };
}
