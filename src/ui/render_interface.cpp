// RmlUi's render interface over gpu::ui (render_interface.h).

#include "ui/render_interface.h"

#include "log.h"
#include "ui/dds.h"
#include "ui/game_archive.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Vertex.h>

#include <cstring>
#include <limits>

namespace ui
{
    namespace
    {
        // RmlUi's vertex is the bridge's: position, premultiplied colour (red first), texture
        // coordinate. The arrays are copied as they are.
        static_assert(sizeof(Rml::Vertex) == sizeof(gpu::ui::Vertex));
        static_assert(sizeof(Rml::ColourbPremultiplied) == 4);

        struct Geometry
        {
            std::vector<gpu::ui::Vertex> vertices;
            std::vector<uint32_t> indices;
        };

        // Reads a whole file through RmlUi's file interface (file_interface.h: under data/ui).
        bool ReadFile(const Rml::String& path, std::vector<uint8_t>& out)
        {
            Rml::FileInterface* files = Rml::GetFileInterface();
            Rml::FileHandle file = files->Open(path);
            if (!file)
                return false;
            out.resize(files->Length(file));
            bool read = files->Read(out.data(), out.size(), file) == out.size();
            files->Close(file);
            return read;
        }
    }

    RenderInterface::RenderInterface(GameArchive& archive) : archive_(archive) {}

    void RenderInterface::BeginFrame(uint32_t width, uint32_t height)
    {
        frame_ = {};
        frame_.width = width;
        frame_.height = height;
    }

    gpu::ui::Frame RenderInterface::TakeFrame()
    {
        return std::exchange(frame_, {});
    }

    Rml::CompiledGeometryHandle RenderInterface::CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
        Rml::Span<const int> indices)
    {
        auto* geometry = new Geometry;
        geometry->vertices.resize(vertices.size());
        if (!vertices.empty())
            std::memcpy(geometry->vertices.data(), vertices.data(), vertices.size() * sizeof(gpu::ui::Vertex));
        geometry->indices.assign(indices.begin(), indices.end());
        return reinterpret_cast<Rml::CompiledGeometryHandle>(geometry);
    }

    void RenderInterface::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation,
        Rml::TextureHandle texture)
    {
        const auto* geometry = reinterpret_cast<const Geometry*>(handle);
        if (geometry == nullptr || geometry->indices.empty())
            return;
        if (frame_.vertices.size() + geometry->vertices.size() > size_t(std::numeric_limits<int32_t>::max()))
        {
            LOG_ONCE("ui: error: a frame of more than 2^31 vertices, the rest is not drawn");
            return;
        }
        gpu::ui::Draw draw;
        draw.firstIndex = uint32_t(frame_.indices.size());
        draw.indexCount = uint32_t(geometry->indices.size());
        draw.baseVertex = int32_t(frame_.vertices.size());
        draw.texture = gpu::ui::Texture(texture);
        draw.scissored = scissored_;
        draw.scissor = scissor_;
        draw.translation[0] = translation.x;
        draw.translation[1] = translation.y;
        std::memcpy(draw.transform, transform_, sizeof(transform_));
        frame_.vertices.insert(frame_.vertices.end(), geometry->vertices.begin(), geometry->vertices.end());
        frame_.indices.insert(frame_.indices.end(), geometry->indices.begin(), geometry->indices.end());
        frame_.draws.push_back(draw);
    }

    void RenderInterface::ReleaseGeometry(Rml::CompiledGeometryHandle handle)
    {
        delete reinterpret_cast<Geometry*>(handle);
    }

    Rml::TextureHandle RenderInterface::LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source)
    {
        std::vector<uint8_t> file;
        size_t prefix = std::strlen(kGamePrefix);
        bool game = source.compare(0, prefix, kGamePrefix) == 0;
        if (game ? !archive_.Read(source.substr(prefix), file) : !ReadFile(source, file))
        {
            Log("ui: error: no image %s", source.c_str());
            return 0;
        }
        gpu::ui::TextureDesc desc;
        std::string error;
        if (!ParseDds(file, desc, error))
        {
            Log("ui: error: the image %s: %s", source.c_str(), error.c_str());
            return 0;
        }
        dimensions = Rml::Vector2i(int(desc.width), int(desc.height));
        return Rml::TextureHandle(gpu::ui::CreateTexture(std::move(desc)));
    }

    Rml::TextureHandle RenderInterface::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions)
    {
        if (dimensions.x <= 0 || dimensions.y <= 0 || source.size() != size_t(dimensions.x) * size_t(dimensions.y) * 4)
            return 0;
        gpu::ui::TextureDesc desc;
        desc.format = gpu::ui::Format::Rgba8Premultiplied;
        desc.width = uint32_t(dimensions.x);
        desc.height = uint32_t(dimensions.y);
        desc.data.assign(source.begin(), source.end());
        return Rml::TextureHandle(gpu::ui::CreateTexture(std::move(desc)));
    }

    void RenderInterface::ReleaseTexture(Rml::TextureHandle texture)
    {
        gpu::ui::ReleaseTexture(gpu::ui::Texture(texture));
    }

    void RenderInterface::EnableScissorRegion(bool enable)
    {
        scissored_ = enable;
    }

    void RenderInterface::SetScissorRegion(Rml::Rectanglei region)
    {
        scissor_ = { region.p0.x, region.p0.y, region.p1.x, region.p1.y };
    }

    void RenderInterface::SetTransform(const Rml::Matrix4f* transform)
    {
        static constexpr float kIdentity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        // Column-major, as the bridge's (RMLUI_MATRIX_ROW_MAJOR is off).
        std::memcpy(transform_, transform != nullptr ? transform->data() : kIdentity, sizeof(transform_));
    }
}
