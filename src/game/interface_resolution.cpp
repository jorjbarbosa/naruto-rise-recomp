// The interface's movies at the internal resolution: GFx viewports scaled by the render scale.
//
// GFx tessellates the movies' curves and draws the anti-aliased strip along their edges in viewport
// pixels, and every movie's viewport is its 1280x720 stage (GFx's SetLevelMovie sets it; the game never
// does). A viewport the render scale times larger makes those edges as fine as the host pixels while
// nothing moves: the visible frame stays the whole stage, and the engine's renderer places a movie by
// that frame and the item's share of the screen. The text and the images are textures of a fixed size
// and do not change (docs/development/plans/image-quality/e7-interface-resolution.md).
//
// The scale is the render scale's vertical one: a pixel scale above the host's would make the edge
// strip narrower than a host pixel, and the interface is drawn into a single-sampled target. A movie
// takes the scale when GFx sets its viewport, and again before a display that finds the scale changed.
// No movie listens to Stage.onResize, which SetViewport raises on a change of size.
//
// Threading: GFx sets a viewport on the thread that creates the movie, the engine displays it on the
// main thread, and it is destroyed on either; the table of viewports is under a lock.

#include "game/addresses.h"
#include "game/guest_call.h"
#include "gpu/gpu.h"
#include "memory.h"

#include <ppc_config.h>
#include <ppc_context.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <unordered_map>

DECLARE_ORIGINAL(GFX_SET_VIEWPORT);
DECLARE_ORIGINAL(GFX_MOVIE_DESTROY);
DECLARE_ORIGINAL(JADE_MOVIE_DISPLAY);

namespace game
{
    namespace
    {
        using namespace jade;

        // The guest stack SetViewport runs on when a display calls it again (GuestCall): this far below
        // the display's frame, with the viewport it is handed above its own frame.
        constexpr uint32_t kStackGap = 256;
        constexpr uint32_t kScratch = 128;

        // The scale is taken in steps of 1/80, which keep 1280 and 720 (16 and 9 times 80) whole: with
        // the sides rounded apart, a viewport off 16:9 would make GFx's ShowAll frame a little larger
        // than the stage on one axis, and every item would shrink by a fraction of a pixel.
        constexpr float kScaleSteps = 80.0f;

        // A movie's viewport as GFx set it, and the scale it was last given at.
        struct Viewport
        {
            std::array<uint32_t, gfx_viewport::kSize / 4> words{};
            float scale = 1.0f;
        };

        // By the movie's address, from its first SetViewport to its destruction.
        std::mutex g_mutex;
        std::unordered_map<uint32_t, Viewport> g_viewports;

        // The integer fields of a viewport, in pixels.
        constexpr uint32_t kPixelFields[] = { gfx_viewport::kBufferWidth, gfx_viewport::kBufferHeight,
            gfx_viewport::kLeft, gfx_viewport::kTop, gfx_viewport::kWidth, gfx_viewport::kHeight,
            gfx_viewport::kScissorLeft, gfx_viewport::kScissorTop, gfx_viewport::kScissorWidth,
            gfx_viewport::kScissorHeight };

        float Scale()
        {
            return std::round(gpu::RenderScale() * kScaleSteps) / kScaleSteps;
        }

        // Writes the viewport, scaled, at a guest address.
        void StoreScaled(uint32_t address, const Viewport& viewport)
        {
            for (size_t i = 0; i < viewport.words.size(); i++)
                mem::Store32(address + uint32_t(4 * i), viewport.words[i]);
            for (uint32_t field : kPixelFields)
            {
                int32_t pixels = int32_t(viewport.words[field / 4]);
                mem::Store32(address + field, uint32_t(int32_t(std::lround(float(pixels) * viewport.scale))));
            }
        }
    }
}

using namespace game;

// GFxMovieRoot::SetViewport (r3 movie, r4 the viewport): replaced so that the movie gets the viewport
// the render scale times larger, and its own is kept for a later change of scale. The caller's
// viewport is scaled in place for the original, which copies it, and put back after.
PPC_FUNC(GFX_SET_VIEWPORT)
{
    const uint32_t movie = ctx.r3.u32;
    const uint32_t address = ctx.r4.u32;
    Viewport viewport;
    for (size_t i = 0; i < viewport.words.size(); i++)
        viewport.words[i] = mem::Load32(address + uint32_t(4 * i));
    viewport.scale = Scale();
    {
        std::lock_guard lock(g_mutex);
        g_viewports[movie] = viewport;
    }
    StoreScaled(address, viewport);
    ORIGINAL(GFX_SET_VIEWPORT)(ctx, base);
    for (size_t i = 0; i < viewport.words.size(); i++)
        mem::Store32(address + uint32_t(4 * i), viewport.words[i]);
}

// ~GFxMovieRoot (r3 movie): replaced so that the movie's viewport leaves the table.
PPC_FUNC(GFX_MOVIE_DESTROY)
{
    {
        std::lock_guard lock(g_mutex);
        g_viewports.erase(ctx.r3.u32);
    }
    ORIGINAL(GFX_MOVIE_DESTROY)(ctx, base);
}

// The engine's display of a movie (r3 the wrapper): replaced so that a movie whose scale is not the
// render scale's any more gets its viewport again first.
PPC_FUNC(JADE_MOVIE_DISPLAY)
{
    const uint32_t movie = mem::Load32(ctx.r3.u32 + jade::movie::kView);
    const float scale = Scale();
    if (movie != 0)
    {
        Viewport viewport;
        bool changed = false;
        {
            std::lock_guard lock(g_mutex);
            auto it = g_viewports.find(movie);
            if (it != g_viewports.end() && it->second.scale != scale)
            {
                it->second.scale = scale;
                viewport = it->second;
                changed = true;
            }
        }
        if (changed)
        {
            GuestCall guest(ctx, kStackGap, kScratch);
            PPCContext& call = guest.Context();
            StoreScaled(guest.Scratch(), viewport);
            call.r3.u64 = movie;
            call.r4.u64 = guest.Scratch();
            ORIGINAL(GFX_SET_VIEWPORT)(call, base);
        }
    }
    ORIGINAL(JADE_MOVIE_DISPLAY)(ctx, base);
}
