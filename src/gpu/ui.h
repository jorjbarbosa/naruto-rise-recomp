#pragma once

// The host UI's way into the renderer: textures by handle, and frames of triangles the presents draw
// over the game image (renderer/ui_pass.h). Any thread, also before the renderer exists: what is
// sent waits for the first present.

#include <cstdint>
#include <vector>

namespace gpu::ui
{
    enum class Format : uint8_t
    {
        Rgba8Premultiplied, // 4 bytes a texel, red first, colour already multiplied by alpha
        Bc1,                // the game's DDS data: straight alpha, premultiplied when sampled
        Bc2,
        Bc3,
    };

    struct TextureDesc
    {
        Format format = Format::Rgba8Premultiplied;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t levels = 1;
        // The levels from the largest, each a run of rows with no padding: rows of texels, or for the
        // block formats rows of 4x4 blocks. Block formats need a width and height that are multiples
        // of 4.
        std::vector<uint8_t> data;
    };

    // A texture's handle; 0 is none (the draws that name it take the vertex colour alone).
    using Texture = uint32_t;
    constexpr Texture kNoTexture = 0;

    // RmlUi's vertex: position in frame pixels, colour (red first, premultiplied), texture coordinate.
    struct Vertex
    {
        float x = 0.0f;
        float y = 0.0f;
        uint8_t colour[4] = { 255, 255, 255, 255 };
        float u = 0.0f;
        float v = 0.0f;
    };
    static_assert(sizeof(Vertex) == 20);

    struct Scissor
    {
        int32_t left = 0;
        int32_t top = 0;
        int32_t right = 0;
        int32_t bottom = 0;
    };

    // Triangles of the frame's arrays: indices [firstIndex, firstIndex + indexCount), each added to
    // baseVertex. A vertex is moved by the translation, then by the transform.
    struct Draw
    {
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
        int32_t baseVertex = 0;
        Texture texture = kNoTexture;
        bool scissored = false;
        Scissor scissor; // frame pixels, when scissored
        float translation[2] = {};
        // Column-major, as RmlUi's Matrix4f: element (row, column) at [column * 4 + row].
        float transform[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    };

    struct Frame
    {
        // The pixel size the frame was laid out for: the window's. A swap chain of another size
        // stretches the frame over it.
        uint32_t width = 0;
        uint32_t height = 0;
        // How far the game image behind is blurred, 0 (sharp) to 1.
        float blur = 0.0f;
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<Draw> draws; // in drawing order
    };

    // A new texture; the handle is usable at once.
    Texture CreateTexture(TextureDesc desc);
    // The texture goes after the frames submitted before this call. A frame submitted after it
    // must not name it.
    void ReleaseTexture(Texture texture);
    // The frame every present draws until the next one. A frame with no draws and no blur is the UI
    // hidden.
    void Submit(Frame frame);
}
