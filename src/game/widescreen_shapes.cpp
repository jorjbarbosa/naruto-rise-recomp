// The shapes of the interface's movies that reach the edges of a wide picture.
//
// widescreen.cpp draws most movies in a 16:9 frame at the middle of the picture, clipped to it. GFx
// draws a movie shape by shape through the game's renderer (docs/development/plans/wide-interface/
// study.md): SetMatrix and SetVertexData, then DrawIndexedTriList for each fill. For the movies of
// kRules, the shapes a rule chooses are drawn with their matrix changed so that they reach the
// picture's edges, and their records stay out of the frame's clip:
// - spread: the dialogue box, the background of the menus; stretched about the stage's middle by the
//   picture's stretch, so that the stage's width covers the whole picture;
// - edge bands: the side bands of the menus, the panel of a training; stretched from their inner side
//   to the picture's edge, and the art that runs past the stage over them left uncut.
// A repeating bitmap keeps the size of its texels: its texture coordinates follow the new place instead
// of the shape. The other shapes, the texts and the images (DrawBitmaps) stay in the frame.
//
// Threading: a movie is drawn on one thread, from BeginShapes to EndShapes; the state is that thread's.

#include "game/widescreen_shapes.h"

#include "game/addresses.h"
#include "game/guest_call.h"
#include "memory.h"

#include <ppc_config.h>
#include <ppc_context.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <optional>

DECLARE_ORIGINAL(GFX_RENDERER_SET_MATRIX);
DECLARE_ORIGINAL(GFX_RENDERER_SET_VERTICES);
DECLARE_ORIGINAL(GFX_RENDERER_DRAW_SHAPE);

namespace
{
    namespace gfx = game::jade::gfx_renderer;

    // GFx's units: twips, 20 to a pixel of the 1280x720 stage. The movies of kRules have that stage,
    // placed over the whole screen (GFx's frame 0..25600 by 0..14400 twips), so the stage's middle is
    // the picture's.
    constexpr float kTwips = 20.0f;
    constexpr float kStageWidth = 1280.0f * kTwips;
    constexpr float kStageHeight = 720.0f * kTwips;
    constexpr float kStageMiddle = kStageWidth / 2;
    // How far inside the stage a shape's side may stop and still count as against the stage's edge.
    constexpr float kEdgeSlack = 4.0f * kTwips;
    // How far past the picture's edge a band's outer side goes: GFx's anti-aliased fringe along it, a
    // pixel of the stage before, would show as a fade at the picture's edge once the band is stretched.
    constexpr float kBandOvershoot = 8.0f * kTwips;

    enum class Kind
    {
        Spread,    // fills with a texture at least minWidth wide whose top is minTop or lower (stage pixels)
        EdgeBands, // fills with a texture as tall as the stage against one side of it, not both; the
                   // ones that run past a side are only left out of the clip
    };

    // A movie by its name in lower case with backslashes, as widescreen.cpp's rules write it; a final '*'
    // stands for any ending. The first rule that matches is the movie's.
    struct Rule
    {
        std::string_view movie;
        Kind kind;
        float minTop = 0.0f;
        float minWidth = 0.0f;
    };

    constexpr Rule kRules[] = {
        // The dialogue: the box, the spiral strip on top of it and the band of three tiles under it. The
        // portrait, the text and its mask and the button stay.
        { "plv_text_system", Kind::Spread, 540.0f, 500.0f },
        // The paper behind the pause menu, the options and the saves: an image a little wider than the
        // stage. The inner panel stays.
        { "menu\\menu_ingamebackground", Kind::Spread, -std::numeric_limits<float>::infinity(), 1280.0f },
        // The pause menu, the options and the saves: a band against each side. The thin strips and the
        // dark panel beside them stay.
        { "menu\\menu_*", Kind::EdgeBands },
        // A training's panel against the left side.
        { "training\\training_*", Kind::EdgeBands },
    };

    // The guest stack the renderer's SetMatrix runs on when a hook calls it (GuestCall): this far below
    // the hook caller's frame, with the matrix it is handed above its own frame.
    constexpr uint32_t kStackGap = 256;
    constexpr uint32_t kScratch = 128;

    // A 2x3 matrix as GFx writes it: x' = a x + b y + c, y' = d x + e y + f.
    struct Affine
    {
        float a = 1.0f, b = 0.0f, c = 0.0f;
        float d = 0.0f, e = 1.0f, f = 0.0f;
    };

    // p after q.
    Affine Compose(const Affine& p, const Affine& q)
    {
        return { p.a * q.a + p.b * q.d, p.a * q.b + p.b * q.e, p.a * q.c + p.b * q.f + p.c,
            p.d * q.a + p.e * q.d, p.d * q.b + p.e * q.e, p.d * q.c + p.e * q.f + p.f };
    }

    // The inverse; the identity for a matrix that flattens the shape.
    Affine Inverse(const Affine& m)
    {
        float det = m.a * m.e - m.b * m.d;
        if (det == 0.0f)
            return {};
        return { m.e / det, -m.b / det, (m.b * m.f - m.c * m.e) / det, -m.d / det, m.a / det, (m.c * m.d - m.a * m.f) / det };
    }

    // The horizontal map that takes stage x from to to and from2 to to2.
    Affine MapX(float from, float to, float from2, float to2)
    {
        float scale = (to2 - to) / (from2 - from);
        return { scale, 0.0f, to - scale * from, 0.0f, 1.0f, 0.0f };
    }

    float LoadFloat(uint32_t address)
    {
        return std::bit_cast<float>(mem::Load32(address));
    }

    void StoreFloat(uint32_t address, float value)
    {
        mem::Store32(address, std::bit_cast<uint32_t>(value));
    }

    // The bitmap's texture coordinates of the renderer, as two rows of a matrix.
    Affine LoadTexture(uint32_t renderer)
    {
        return { LoadFloat(renderer + gfx::kTextureU[0]), LoadFloat(renderer + gfx::kTextureU[1]),
            LoadFloat(renderer + gfx::kTextureU[2]), LoadFloat(renderer + gfx::kTextureV[0]),
            LoadFloat(renderer + gfx::kTextureV[1]), LoadFloat(renderer + gfx::kTextureV[2]) };
    }

    void StoreTexture(uint32_t renderer, const Affine& m)
    {
        const float row[6] = { m.a, m.b, m.c, m.d, m.e, m.f };
        for (int i = 0; i < 3; i++)
        {
            StoreFloat(renderer + gfx::kTextureU[i], row[i]);
            StoreFloat(renderer + gfx::kTextureV[i], row[3 + i]);
        }
    }

    bool Matches(std::string_view pattern, std::string_view movie)
    {
        bool prefix = pattern.ends_with('*');
        return prefix ? movie.starts_with(pattern.substr(0, pattern.size() - 1)) : movie == pattern;
    }

    // What the movie being drawn on this thread has handed its renderer so far.
    struct State
    {
        const Rule* rule = nullptr; // none: the movie has no rule, or nothing is being drawn
        float stretch = 1.0f;
        bool hasMatrix = false;     // a SetMatrix since BeginShapes...
        Affine matrix;              // ...and the last one
        uint32_t vertices = 0;      // the last SetVertexData: guest address, count and stride
        uint32_t vertexCount = 0;
        uint32_t vertexStride = 0;
        bool drawingWide = false;
    };
    thread_local State t_state;

    // A shape's extent on the stage, in twips.
    struct Extent
    {
        float left = std::numeric_limits<float>::max();
        float right = std::numeric_limits<float>::lowest();
        float top = std::numeric_limits<float>::max();
        float bottom = std::numeric_limits<float>::lowest();
    };

    // The extent of vertices first..first + count with the current matrix.
    Extent Measure(const State& state, uint32_t first, uint32_t count)
    {
        Extent extent;
        uint32_t end = std::min(first + count, state.vertexCount);
        for (uint32_t i = first; i < end; i++)
        {
            uint32_t vertex = state.vertices + i * state.vertexStride;
            float x = int16_t(mem::Load16(vertex)), y = int16_t(mem::Load16(vertex + 2));
            float stageX = state.matrix.a * x + state.matrix.b * y + state.matrix.c;
            float stageY = state.matrix.d * x + state.matrix.e * y + state.matrix.f;
            extent.left = std::min(extent.left, stageX);
            extent.right = std::max(extent.right, stageX);
            extent.top = std::min(extent.top, stageY);
            extent.bottom = std::max(extent.bottom, stageY);
        }
        return extent;
    }

    // Where on the stage the shape the renderer is about to draw (vertices first..first + count) goes,
    // as a map of the stage; none when the movie's rule leaves it in the frame. Only fills with a
    // texture are moved, and only with the matrix the movie handed the renderer, which can be put back.
    std::optional<Affine> WidePlace(const State& state, uint32_t renderer, uint32_t first, uint32_t count)
    {
        uint32_t fill = mem::Load32(renderer + gfx::kFillMode);
        if ((fill != gfx::kFillBitmap && fill != gfx::kFillGouraud) || mem::Load32(renderer + gfx::kTexture) == 0)
            return std::nullopt;
        if (!state.hasMatrix || state.vertices == 0 || first >= state.vertexCount)
            return std::nullopt;
        Extent extent = Measure(state, first, count);
        // The picture's edges on the stage.
        float pictureLeft = kStageMiddle - kStageMiddle * state.stretch;
        float pictureRight = kStageMiddle + kStageMiddle * state.stretch;
        switch (state.rule->kind)
        {
        case Kind::Spread:
            if (extent.top / kTwips < state.rule->minTop || (extent.right - extent.left) / kTwips < state.rule->minWidth)
                return std::nullopt;
            return MapX(0.0f, pictureLeft, kStageWidth, pictureRight);
        case Kind::EdgeBands:
        {
            bool atLeft = extent.left <= kEdgeSlack, atRight = extent.right >= kStageWidth - kEdgeSlack;
            bool tall = extent.top <= kEdgeSlack && extent.bottom >= kStageHeight - kEdgeSlack;
            if (tall && atLeft && !atRight)
                return MapX(extent.left, pictureLeft - kBandOvershoot, extent.right, extent.right);
            if (tall && atRight && !atLeft)
                return MapX(extent.left, extent.left, extent.right, pictureRight + kBandOvershoot);
            // Art that runs past the stage's side over a band (a training's title stroke) shows its own
            // end instead of the frame's cut.
            if (extent.left < -kEdgeSlack || extent.right > kStageWidth + kEdgeSlack)
                return Affine{};
            return std::nullopt;
        }
        }
        return std::nullopt;
    }

    // Hands the renderer a matrix through its own SetMatrix, called from the hook with context ctx; the
    // call hands the floating-point mode it leaves back to ctx before returning.
    void SetMatrix(PPCContext& ctx, uint8_t* base, uint32_t renderer, const Affine& m)
    {
        game::GuestCall call(ctx, kStackGap, kScratch);
        const float words[6] = { m.a, m.b, m.c, m.d, m.e, m.f };
        for (int i = 0; i < 6; i++)
            StoreFloat(call.Scratch() + 4 * i, words[i]);
        PPCContext& callCtx = call.Context();
        callCtx.r3.u64 = renderer;
        callCtx.r4.u64 = call.Scratch();
        ORIGINAL(GFX_RENDERER_SET_MATRIX)(callCtx, base);
    }
}

void game::widescreen::BeginShapes(std::string_view movie, float stretch)
{
    t_state = {};
    for (const Rule& rule : kRules)
        if (Matches(rule.movie, movie))
        {
            t_state.rule = &rule;
            break;
        }
    t_state.stretch = stretch;
}

void game::widescreen::EndShapes()
{
    t_state = {};
}

bool game::widescreen::DrawingWideShape()
{
    return t_state.drawingWide;
}

// The renderer's SetMatrix (825E8490; r3 the renderer, r4 the shape's matrix to the stage). Hooked to
// keep the matrix of a movie with rules, which DrawIndexedTriList's hook measures the shape with.
PPC_FUNC(GFX_RENDERER_SET_MATRIX)
{
    if (t_state.rule != nullptr)
    {
        uint32_t m = ctx.r4.u32;
        t_state.matrix = { LoadFloat(m), LoadFloat(m + 4), LoadFloat(m + 8), LoadFloat(m + 12), LoadFloat(m + 16), LoadFloat(m + 20) };
        t_state.hasMatrix = true;
    }
    ORIGINAL(GFX_RENDERER_SET_MATRIX)(ctx, base);
}

// The renderer's SetVertexData (825E9130; r3 the renderer, r4 the vertices, r5 their count, r6 the
// format). Hooked to keep where the vertices of a movie with rules are: GFx's own copy, which stays
// while the shapes that use it are drawn.
PPC_FUNC(GFX_RENDERER_SET_VERTICES)
{
    if (t_state.rule != nullptr)
    {
        uint32_t format = ctx.r6.u32;
        t_state.vertices = ctx.r4.u32;
        t_state.vertexCount = ctx.r5.u32;
        t_state.vertexStride = format == gfx::kVertexXY ? 4 : format == gfx::kVertexXYColor ? 8 : format == gfx::kVertexXYColorFactor ? 12 : 0;
        if (t_state.vertexStride == 0)
            t_state.vertices = 0;
    }
    ORIGINAL(GFX_RENDERER_SET_VERTICES)(ctx, base);
}

// The renderer's DrawIndexedTriList (825E8060; r3 the renderer, r4 the base vertex, r5 the first, r6
// the count, r7 the first index, r8 the triangles): one fill of a shape, with the last SetMatrix. Hooked
// to draw a chosen shape at its place in the wide picture: its matrix mapped there and, for a repeating
// bitmap, texture coordinates made from that place, both put back after the draw.
PPC_FUNC(GFX_RENDERER_DRAW_SHAPE)
{
    State& state = t_state;
    uint32_t renderer = ctx.r3.u32;
    std::optional<Affine> place;
    if (state.rule != nullptr)
        place = WidePlace(state, renderer, ctx.r4.u32 + ctx.r5.u32, ctx.r6.u32);
    if (!place)
    {
        ORIGINAL(GFX_RENDERER_DRAW_SHAPE)(ctx, base);
        return;
    }
    Affine shape = state.matrix;
    bool repeats = mem::Load32(renderer + gfx::kWrap) == gfx::kWrapRepeat;
    Affine texture = LoadTexture(renderer);
    SetMatrix(ctx, base, renderer, Compose(*place, shape));
    if (repeats)
        StoreTexture(renderer, Compose(texture, Compose(Inverse(shape), Compose(*place, shape))));
    state.drawingWide = true;
    ORIGINAL(GFX_RENDERER_DRAW_SHAPE)(ctx, base);
    state.drawingWide = false;
    if (repeats)
        StoreTexture(renderer, texture);
    SetMatrix(ctx, base, renderer, shape);
}
