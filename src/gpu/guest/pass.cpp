// Clears, tiled passes and resolves. The D3D functions run as the original (their CPU state and
// fences are kept, their packets land in the discard segment) and the hooks post the matching
// host operations. While a pass is tiled the surfaces are backings of the whole tiled area (see
// Device), so the per-tile resolves copy their regions of them.

#include "gpu/gpu.h"
#include "gpu/guest/addresses.h"
#include "gpu/guest/device.h"
#include "gpu/guest/resources.h"
#include "gpu/guest/textures.h"
#include "gpu/renderer/renderer.h"
#include "kernel/kernel.h"

#include <algorithm>
#include <climits>
#include <optional>

namespace gpu::guest
{
    namespace
    {
        // D3DCLEAR flags of ClearF.
        constexpr uint32_t kClearTargets = 0xF; // bit i: render target i
        constexpr uint32_t kClearDepth = 0x10;
        constexpr uint32_t kClearStencil = 0x20;

        // D3DRESOLVE flags of Resolve.
        constexpr uint32_t kResolveSourceMask = 7; // 0..3: render target, 4: depth-stencil
        constexpr uint32_t kResolveSourceDepth = 4;
        constexpr uint32_t kResolveSampleShift = 4; // 3 bits, see SampleOf
        constexpr uint32_t kResolveClearColor = 0x100;
        constexpr uint32_t kResolveClearDepth = 0x200;

        struct ClearArgs
        {
            uint32_t flags = 0;
            std::array<float, 4> color{};
            float depth = 0.0f;
            uint8_t stencil = 0;
            std::optional<Rect> rect; // none: the whole surfaces
        };

        std::array<float, 4> LoadColor(uint32_t vector)
        {
            std::array<float, 4> color{}; // a null color clears to zero, as in the original
            if (vector != 0)
                for (uint32_t i = 0; i < 4; i++)
                    color[i] = LoadFloat(vector + 4 * i);
            return color;
        }

        Rect LoadRect(uint32_t rect)
        {
            return { int32_t(mem::Load32(rect)), int32_t(mem::Load32(rect + 4)), int32_t(mem::Load32(rect + 8)),
                int32_t(mem::Load32(rect + 12)) };
        }

        // The pass state. The D3D device is driven by one guest thread (the render thread).
        class Passes
        {
        public:
            // ClearF(device, flags, rect, color, z, stencil).
            void Clear(uint32_t device, const ClearArgs& args)
            {
                if (deferClear_)
                {
                    deferredClear_ = args;
                    return;
                }
                ClearArgs clipped = args;
                clipped.rect = ClipToViewport(device, args.rect);
                if (!clipped.rect->Empty())
                    Post(BuildClear(clipped));
            }

            // D3DDevice_BeginTiling, around the original.
            void BeginTiling(uint32_t device, const std::function<void()>& original)
            {
                // The original clears the first tile through ClearF; the host clears the backings.
                deferClear_ = true;
                original();
                deferClear_ = false;
                uint32_t width = mem::Load32(device + dev::kTilingWidth);
                uint32_t height = mem::Load32(device + dev::kTilingHeight);
                GetDevice().BeginTiling(width, height);
                LOG_ONCE("gpu: first tiled pass, %ux%u", width, height);
                if (deferredClear_)
                {
                    deferredClear_->rect.reset();
                    Post(BuildClear(*deferredClear_));
                    deferredClear_.reset();
                }
            }

            // D3DDevice_EndTiling(device, flags, rects, dest, color, z), around the original.
            void EndTiling(uint32_t device, uint32_t flags, uint32_t dest, const ClearArgs& finalClear,
                const std::function<void()>& original)
            {
                // The original resolves every tile through Resolve and clears the source after all
                // but the last one, for the replay of the next tile. Those clears are skipped.
                inEndTiling_ = true;
                original();
                inEndTiling_ = false;
                GetDevice().EndTiling();
                // The last tile's clear leaves the EDRAM surfaces cleared: here, the guest surfaces.
                // The original passes stencil 0 to that resolve. Without a destination it resolves
                // nothing and clears nothing.
                if (dest != 0)
                    PostResolveClear(flags, finalClear);
            }

            // Resolve(device, flags, rect, dest, point, level, slice, color, z, stencil).
            void Resolve(uint32_t device, uint32_t flags, uint32_t rect, uint32_t dest, uint32_t point,
                const ClearArgs& clear)
            {
                uint32_t source = flags & kResolveSourceMask;
                if (source > kResolveSourceDepth)
                {
                    LOG_ONCE("gpu: resolve source %u is not covered", source);
                    return;
                }
                if (dest == 0)
                {
                    // The original reads the destination header unconditionally.
                    LOG_ONCE("gpu: resolve without a destination skipped");
                    return;
                }
                TextureFetch texture = ReadTextureFetch(dest + res::kTextureFetch);
                // Without a rect the original copies the destination's size from the origin.
                Rect sourceRect = rect != 0 ? LoadRect(rect) : Rect{ 0, 0, int32_t(texture.width), int32_t(texture.height) };

                if (std::optional<SurfaceDesc> surface = GetDevice().BoundSurface(SlotOffset(source)))
                {
                    if (texture.is2D && DestinationCovered(surface->depth, texture.format))
                    {
                        ResolveOp op;
                        op.source = *surface;
                        op.sample = SampleOf(*surface, (flags >> kResolveSampleShift) & 7);
                        op.sourceRect = sourceRect;
                        if (point != 0)
                        {
                            op.destX = mem::Load32(point);
                            op.destY = mem::Load32(point + 4);
                        }
                        op.destAddress = texture.address;
                        op.destWidth = texture.width;
                        op.destHeight = texture.height;
                        op.destHeader = dest;
                        if (Renderer* renderer = ActiveRenderer())
                        {
                            renderer->Resolve(op);
                            NoteResolved(texture.address, dest);
                        }
                    }
                    else
                    {
                        LOG_ONCE("gpu: resolve to a texture of format %u%s is not covered", texture.format,
                            texture.is2D ? "" : " (not 2D)");
                    }
                }

                // The clear happens even when the copy is not covered.
                if (!inEndTiling_)
                {
                    ClearArgs region = clear;
                    region.rect = sourceRect; // the copy clears the region it copied
                    PostResolveClear(flags, region);
                }
            }

        private:
            static uint32_t SlotOffset(uint32_t source)
            {
                return source == kResolveSourceDepth ? dev::kDepthStencil : dev::kRenderTargets + 4 * source;
            }

            // Color resolves keep the surface's colors (RGBA8) whatever the destination's format; a
            // k_5_6_5 destination (the save thumbnail) drops the alpha when it is read back, and a k_8
            // one (the per-character shadow maps) is sampled through its red channel.
            static bool DestinationCovered(bool depth, uint32_t format)
            {
                return depth ? format == format::k24_8 || format == format::k24_8Float
                             : format == format::k8_8_8_8 || format == format::k5_6_5 || format == format::k8;
            }

            // The sample a resolve copies, from the sample select of its flags (copy_sample_select
            // + 1; 0 lets the original pick every sample of the source).
            static int32_t SampleOf(const SurfaceDesc& surface, uint32_t select)
            {
                if (select >= 1 && select <= 4)
                    return int32_t(select - 1);
                if (surface.samples == 1)
                    return 0;
                if (surface.depth)
                {
                    LOG_ONCE("gpu: depth resolve of several samples copies sample 0");
                    return 0;
                }
                // 5 and 6 select a pair of samples: all of a 2x source, half of a 4x one.
                if ((select == 5 || select == 6) && surface.samples == 4)
                    LOG_ONCE("gpu: resolve of a sample pair averages all 4 samples");
                return -1;
            }

            // The clear area of ClearF: its rect (or everything) within the viewport, and within
            // the scissor rectangle when scissoring is on.
            static Rect ClipToViewport(uint32_t device, const std::optional<Rect>& rect)
            {
                Rect area = rect.value_or(Rect{ 0, 0, INT_MAX, INT_MAX });
                int32_t x = int32_t(LoadFloat(device + dev::kViewport));
                int32_t y = int32_t(LoadFloat(device + dev::kViewport + 4));
                int32_t width = int32_t(LoadFloat(device + dev::kViewport + 8));
                int32_t height = int32_t(LoadFloat(device + dev::kViewport + 12));
                area = Intersect(area, { x, y, x + width, y + height });
                if (mem::Load32(device + dev::kScissorEnable) != 0)
                    area = Intersect(area, LoadRect(device + dev::kScissor));
                return area;
            }

            static ClearOp BuildClear(const ClearArgs& args)
            {
                const Device& device = GetDevice();
                ClearOp op;
                for (uint32_t i = 0; i < dev::kRenderTargetCount; i++)
                    if ((args.flags & (1u << i)) != 0)
                        op.colors[i] = device.BoundSurface(dev::kRenderTargets + 4 * i).value_or(SurfaceDesc{});
                if ((args.flags & (kClearDepth | kClearStencil)) != 0)
                {
                    op.depth = device.BoundSurface(dev::kDepthStencil).value_or(SurfaceDesc{});
                    op.clearDepth = (args.flags & kClearDepth) != 0;
                    op.clearStencil = (args.flags & kClearStencil) != 0;
                }
                op.color = args.color;
                op.depthValue = args.depth;
                op.stencil = args.stencil;
                op.rect = args.rect;
                return op;
            }

            // The clear flags of a resolve: its source render target, the depth-stencil or both.
            static void PostResolveClear(uint32_t flags, const ClearArgs& args)
            {
                uint32_t source = flags & kResolveSourceMask;
                ClearArgs clear = args;
                clear.flags = 0;
                if ((flags & kResolveClearColor) != 0 && source < kResolveSourceDepth)
                    clear.flags |= 1u << source;
                if ((flags & kResolveClearDepth) != 0)
                    clear.flags |= kClearDepth | kClearStencil;
                if (clear.flags != 0)
                    Post(BuildClear(clear));
            }

            static void Post(const ClearOp& op)
            {
                bool any = op.depth.key != 0;
                for (const SurfaceDesc& color : op.colors)
                    any |= color.key != 0;
                if (!any)
                    return;
                if (Renderer* renderer = ActiveRenderer())
                    renderer->Clear(op);
            }

            bool deferClear_ = false;
            std::optional<ClearArgs> deferredClear_;
            bool inEndTiling_ = false;
        };

        Passes g_passes;
    }
}

using namespace gpu::guest;

DECLARE_ORIGINAL(D3D_CLEAR);
DECLARE_ORIGINAL(D3D_BEGIN_TILING);
DECLARE_ORIGINAL(D3D_END_TILING);
DECLARE_ORIGINAL(D3D_RESOLVE);

// D3DDevice_ClearF(device, flags, rect, color, z, stencil).
PPC_FUNC(D3D_CLEAR)
{
    ClearArgs args;
    args.flags = ctx.r4.u32;
    if (ctx.r5.u32 != 0)
        args.rect = LoadRect(ctx.r5.u32);
    args.color = LoadColor(ctx.r6.u32);
    args.depth = float(ctx.f1.f64);
    args.stencil = uint8_t(ctx.r8.u32);
    uint32_t device = ctx.r3.u32;
    ORIGINAL(D3D_CLEAR)(ctx, base);
    g_passes.Clear(device, args);
}

// D3DDevice_BeginTiling(device, flags, count, tiles, color, z, -, stencil). LTCG specialized it:
// it ignores count and tiles and reads its three tiles from a fixed table.
PPC_FUNC(D3D_BEGIN_TILING)
{
    g_passes.BeginTiling(ctx.r3.u32, [&] { ORIGINAL(D3D_BEGIN_TILING)(ctx, base); });
}

// D3DDevice_EndTiling(device, flags, rects, dest, color, z, ...): resolves every tile into dest.
PPC_FUNC(D3D_END_TILING)
{
    uint32_t device = ctx.r3.u32;
    uint32_t flags = ctx.r4.u32;
    uint32_t dest = ctx.r6.u32;
    ClearArgs finalClear;
    finalClear.color = LoadColor(ctx.r7.u32);
    finalClear.depth = float(ctx.f1.f64);
    g_passes.EndTiling(device, flags, dest, finalClear, [&] { ORIGINAL(D3D_END_TILING)(ctx, base); });
}

// D3DDevice_Resolve(device, flags, rect, dest, point, level, slice, color, z, stencil, parameters).
// z takes the ninth argument slot, so the stencil is the tenth (the caller's stack).
PPC_FUNC(D3D_RESOLVE)
{
    uint32_t device = ctx.r3.u32;
    uint32_t flags = ctx.r4.u32;
    uint32_t rect = ctx.r5.u32;
    uint32_t dest = ctx.r6.u32;
    uint32_t point = ctx.r7.u32;
    ClearArgs clear;
    clear.color = LoadColor(ctx.r10.u32);
    clear.depth = float(ctx.f1.f64);
    clear.stencil = uint8_t(kernel::StackArg32(ctx, 9));
    ORIGINAL(D3D_RESOLVE)(ctx, base);
    g_passes.Resolve(device, flags, rect, dest, point, clear);
}
