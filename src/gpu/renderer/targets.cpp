// Guest surfaces and resolved texture memory: creation, clears and resolves.

#include "gpu/renderer/targets.h"

#include "gpu/renderer/context.h"
#include "gpu/renderer/passes.h"

#include <algorithm>
#include <optional>

namespace gpu
{
    namespace
    {
        constexpr plume::RenderFormat kResolvedDepthFormat = plume::RenderFormat::R32_FLOAT;

        plume::RenderMultisampling Multisampling(uint32_t samples)
        {
            plume::RenderMultisampling multisampling;
            multisampling.sampleCount = plume::RenderSampleCounts(samples); // the bits are the counts
            return multisampling;
        }

        void Barrier(plume::RenderCommandList& list, plume::RenderBarrierStages stages, plume::RenderTexture* texture,
            plume::RenderTextureLayout layout)
        {
            list.barriers(stages, plume::RenderTextureBarrier(texture, layout));
        }
    }

    Targets::Surface& Targets::EnsureSurface(const SurfaceDesc& desc)
    {
        auto it = surfaces_.find(desc.key);
        if (it != surfaces_.end())
        {
            if (it->second.desc == desc)
                return it->second;
            RetireSurface(desc.key);
        }

        plume::RenderDevice& device = context_.Device();
        Surface surface;
        surface.desc = desc;
        if (desc.depth)
        {
            surface.texture = device.createTexture(
                plume::RenderTextureDesc::DepthTarget(desc.width, desc.height, kDepthFormat, Multisampling(desc.samples)));
            surface.view = surface.texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(kDepthFormat));
            surface.framebuffer = device.createFramebuffer(plume::RenderFramebufferDesc(nullptr, 0, surface.texture.get()));
        }
        else
        {
            surface.texture = device.createTexture(
                plume::RenderTextureDesc::ColorTarget(desc.width, desc.height, kColorFormat, Multisampling(desc.samples)));
            surface.view = surface.texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(kColorFormat));
            const plume::RenderTexture* attachment = surface.texture.get();
            surface.framebuffer = device.createFramebuffer(plume::RenderFramebufferDesc(&attachment, 1));
        }
        return surfaces_.emplace(desc.key, std::move(surface)).first->second;
    }

    ResolvedTexture& Targets::EnsureResolved(uint32_t address, uint32_t width, uint32_t height, bool depth)
    {
        ResolvedTexture& resolved = resolved_[address];
        if (resolved.texture && resolved.width == width && resolved.height == height && resolved.depth == depth)
            return resolved;
        if (resolved.texture)
            context_.Retire(std::make_shared<ResolvedTexture>(std::move(resolved)));

        plume::RenderDevice& device = context_.Device();
        resolved = {};
        resolved.width = width;
        resolved.height = height;
        resolved.depth = depth;
        resolved.format = depth ? kResolvedDepthFormat : kColorFormat;
        // A render target too: the resolve passes draw into it.
        resolved.texture = device.createTexture(plume::RenderTextureDesc::ColorTarget(width, height, resolved.format));
        const plume::RenderTexture* attachment = resolved.texture.get();
        resolved.framebuffer = device.createFramebuffer(plume::RenderFramebufferDesc(&attachment, 1));
        resolved.sampling = passes_.CreateTextureSet(*resolved.texture, nullptr);
        resolved.descriptor = TextureDescriptor(descriptors_, TextureSet::k2D, *resolved.texture);
        return resolved;
    }

    void Targets::RetireSurface(uint32_t key)
    {
        auto it = surfaces_.find(key);
        if (it == surfaces_.end())
            return;
        context_.Retire(std::make_shared<Surface>(std::move(it->second)));
        surfaces_.erase(it);
        for (auto framebuffer = drawFramebuffers_.begin(); framebuffer != drawFramebuffers_.end();)
        {
            if (framebuffer->first.first == key || framebuffer->first.second == key)
            {
                context_.Retire(std::shared_ptr<plume::RenderFramebuffer>(std::move(framebuffer->second)));
                framebuffer = drawFramebuffers_.erase(framebuffer);
            }
            else
            {
                ++framebuffer;
            }
        }
    }

    void Targets::BindForDraw(plume::RenderCommandList& list, const SurfaceDesc& color, const SurfaceDesc& depth)
    {
        // Both first: creating one must not retire the framebuffer of the other.
        Surface* colorSurface = color.key != 0 ? &EnsureSurface(color) : nullptr;
        Surface* depthSurface = depth.key != 0 ? &EnsureSurface(depth) : nullptr;
        if (colorSurface != nullptr)
            Barrier(list, plume::RenderBarrierStage::GRAPHICS, colorSurface->texture.get(), plume::RenderTextureLayout::COLOR_WRITE);
        if (depthSurface != nullptr)
            Barrier(list, plume::RenderBarrierStage::GRAPHICS, depthSurface->texture.get(), plume::RenderTextureLayout::DEPTH_WRITE);

        std::unique_ptr<plume::RenderFramebuffer>& framebuffer = drawFramebuffers_[{ color.key, depth.key }];
        if (!framebuffer)
        {
            const plume::RenderTexture* attachment = colorSurface != nullptr ? colorSurface->texture.get() : nullptr;
            framebuffer = context_.Device().createFramebuffer(plume::RenderFramebufferDesc(
                colorSurface != nullptr ? &attachment : nullptr, colorSurface != nullptr ? 1 : 0,
                depthSurface != nullptr ? depthSurface->texture.get() : nullptr));
        }
        list.setFramebuffer(framebuffer.get());
    }

    ResolvedTexture* Targets::Resolved(uint32_t address)
    {
        auto it = resolved_.find(address);
        return it != resolved_.end() && it->second.written ? &it->second : nullptr;
    }

    void Targets::Clear(const ClearOp& op)
    {
        auto area = [&](const SurfaceDesc& desc) -> std::optional<plume::RenderRect> {
            if (!op.rect)
                return std::nullopt;
            Rect rect = Intersect(*op.rect, { 0, 0, int32_t(desc.width), int32_t(desc.height) });
            return plume::RenderRect(rect.left, rect.top, rect.right, rect.bottom);
        };

        plume::RenderCommandList& list = context_.Commands();
        for (const SurfaceDesc& desc : op.colors)
        {
            if (desc.key == 0)
                continue;
            std::optional<plume::RenderRect> rect = area(desc);
            if (rect && rect->isEmpty())
                continue;
            Surface& surface = EnsureSurface(desc);
            Barrier(list, plume::RenderBarrierStage::GRAPHICS, surface.texture.get(), plume::RenderTextureLayout::COLOR_WRITE);
            list.setFramebuffer(surface.framebuffer.get());
            list.clearColor(0, plume::RenderColor(op.color[0], op.color[1], op.color[2], op.color[3]),
                rect ? &*rect : nullptr, rect ? 1 : 0);
        }
        if (op.depth.key != 0 && (op.clearDepth || op.clearStencil))
        {
            std::optional<plume::RenderRect> rect = area(op.depth);
            if (rect && rect->isEmpty())
                return;
            Surface& surface = EnsureSurface(op.depth);
            Barrier(list, plume::RenderBarrierStage::GRAPHICS, surface.texture.get(), plume::RenderTextureLayout::DEPTH_WRITE);
            list.setFramebuffer(surface.framebuffer.get());
            list.clearDepthStencil(op.clearDepth, op.clearStencil, op.depthValue, op.stencil, rect ? &*rect : nullptr,
                rect ? 1 : 0);
        }
    }

    void Targets::Resolve(const ResolveOp& op)
    {
        // The source rect, within the source and, moved to the destination point, within the
        // destination. A resolve that copies nothing leaves the destination as it is.
        int32_t offsetX = int32_t(op.destX) - op.sourceRect.left;
        int32_t offsetY = int32_t(op.destY) - op.sourceRect.top;
        Rect region = Intersect(op.sourceRect, { 0, 0, int32_t(op.source.width), int32_t(op.source.height) });
        region = Intersect(region,
            { -offsetX, -offsetY, int32_t(op.destWidth) - offsetX, int32_t(op.destHeight) - offsetY });
        if (region.Empty())
            return;
        uint32_t destX = uint32_t(region.left + offsetX);
        uint32_t destY = uint32_t(region.top + offsetY);

        Surface& source = EnsureSurface(op.source);
        ResolvedTexture& dest = EnsureResolved(op.destAddress, op.destWidth, op.destHeight, op.source.depth);
        dest.header = op.destHeader;

        plume::RenderCommandList& list = context_.Commands();
        if (op.source.depth || (op.source.samples > 1 && op.sample >= 0))
        {
            ResolvePass(source, dest, region, destX, destY, op.sample);
        }
        else if (op.source.samples > 1)
        {
            Barrier(list, plume::RenderBarrierStage::COPY, source.texture.get(), plume::RenderTextureLayout::RESOLVE_SOURCE);
            Barrier(list, plume::RenderBarrierStage::COPY, dest.texture.get(), plume::RenderTextureLayout::RESOLVE_DEST);
            const plume::RenderRect rect(region.left, region.top, region.right, region.bottom);
            list.resolveTextureRegion(dest.texture.get(), destX, destY, source.texture.get(), &rect);
        }
        else
        {
            Barrier(list, plume::RenderBarrierStage::COPY, source.texture.get(), plume::RenderTextureLayout::COPY_SOURCE);
            Barrier(list, plume::RenderBarrierStage::COPY, dest.texture.get(), plume::RenderTextureLayout::COPY_DEST);
            const plume::RenderBox box(region.left, region.top, region.right, region.bottom);
            list.copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(dest.texture.get()),
                plume::RenderTextureCopyLocation::Subresource(source.texture.get()), destX, destY, 0, &box);
        }
        dest.written = true;
    }

    void Targets::ResolvePass(Surface& source, ResolvedTexture& dest, const Rect& region, uint32_t destX, uint32_t destY,
        int32_t sample)
    {
        if (!source.sampling)
            source.sampling = passes_.CreateTextureSet(*source.texture, source.view.get());
        uint32_t samples = source.desc.samples;
        Passes::Resolve kind = !source.desc.depth ? Passes::Resolve::ColorSample
            : samples > 1                         ? Passes::Resolve::DepthMultisample
                                                  : Passes::Resolve::Depth;

        plume::RenderCommandList& list = context_.Commands();
        Barrier(list, plume::RenderBarrierStage::GRAPHICS, source.texture.get(), plume::RenderTextureLayout::SHADER_READ);
        Barrier(list, plume::RenderBarrierStage::GRAPHICS, dest.texture.get(), plume::RenderTextureLayout::COLOR_WRITE);
        list.setFramebuffer(dest.framebuffer.get());
        uint32_t width = uint32_t(region.right - region.left);
        uint32_t height = uint32_t(region.bottom - region.top);
        const plume::RenderViewport viewport{ float(destX), float(destY), float(width), float(height) };
        const plume::RenderRect scissor(int32_t(destX), int32_t(destY), int32_t(destX + width), int32_t(destY + height));
        list.setViewports(&viewport, 1);
        list.setScissors(&scissor, 1);
        // A sample the source lacks wraps around, as on a 2x Xenos surface.
        passes_.DrawResolve(list, kind, *source.sampling, region.left - int32_t(destX), region.top - int32_t(destY),
            uint32_t(std::max(sample, 0)) % samples);
    }

    void Targets::ReleaseSurface(uint32_t key)
    {
        // The surface and the backing of its tiled passes.
        for (uint32_t surfaceKey : { key, key | 1 })
            RetireSurface(surfaceKey);
    }

    void Targets::ReleaseTexture(uint32_t address, uint32_t header)
    {
        auto it = resolved_.find(address);
        if (it == resolved_.end() || it->second.header != header)
            return;
        context_.Retire(std::make_shared<ResolvedTexture>(std::move(it->second)));
        resolved_.erase(it);
    }

    void Targets::ReleaseResolved(uint32_t begin, uint32_t end)
    {
        for (auto it = resolved_.begin(); it != resolved_.end();)
        {
            if (it->first < begin || it->first >= end)
            {
                ++it;
                continue;
            }
            context_.Retire(std::make_shared<ResolvedTexture>(std::move(it->second)));
            it = resolved_.erase(it);
        }
    }
}
