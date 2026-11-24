// Guest textures on the host: creation, uploads of their levels and faces, and the descriptors the
// draws sample them through.

#include "gpu/renderer/textures.h"

#include "gpu/renderer/backend.h"
#include "gpu/renderer/context.h"
#include "gpu/renderer/targets.h"
#include "gpu/textures/formats.h"
#include "log.h"


#include <algorithm>
#include <cstring>

namespace gpu
{
    namespace
    {
        constexpr uint8_t kIdentity[4] = { 0, 1, 2, 3 };

        // The view mapping of a fetch swizzle over the host swizzle of the format: a selector of a
        // guest component reads the host component that holds it; 0 and 1 are constants.
        plume::RenderComponentMapping Mapping(uint32_t swizzle, const uint8_t (&host)[4])
        {
            plume::RenderSwizzle components[4];
            for (uint32_t i = 0; i < 4; i++)
            {
                uint32_t select = (swizzle >> (3 * i)) & 7;
                if (select < 4)
                {
                    components[i] = plume::RenderSwizzle(uint32_t(plume::RenderSwizzle::R) + host[select]);
                }
                else if (select == 5)
                {
                    components[i] = plume::RenderSwizzle::ONE;
                }
                else
                {
                    if (select > 5)
                        LOG_ONCE("gpu: texture swizzle selector %u reads as zero", select);
                    components[i] = plume::RenderSwizzle::ZERO;
                }
            }
            return { components[0], components[1], components[2], components[3] };
        }

        // The bytes of guest memory at a GPU address in an upload, and how many follow it.
        const uint8_t* FindMemory(const TextureUpload& upload, uint32_t address, uint32_t& bytes)
        {
            size_t offset = 0;
            for (const auto& [start, size] : upload.ranges)
            {
                if (address >= start && address - start < size)
                {
                    bytes = size - (address - start);
                    return upload.data.data() + offset + (address - start);
                }
                offset += size;
            }
            bytes = 0;
            return nullptr;
        }
    }

    Textures::Texture* Textures::Create(const TextureUpload& upload, const textures::Format& format)
    {
        const textures::Layout& layout = upload.layout;
        plume::RenderTextureDesc desc =
            plume::RenderTextureDesc::Texture2D(layout.width, layout.height, layout.mipMax + 1, format.host);
        desc.arraySize = layout.faces;
        if (layout.dimension == textures::Dimension::kCube)
            desc.flags |= plume::RenderTextureFlag::CUBE;
        auto texture = std::make_unique<Texture>();
        texture->format = &format;
        texture->cube = layout.dimension == textures::Dimension::kCube;
        texture->texture = context_.Device().createTexture(desc);
        if (!IsCreated(texture->texture.get()))
        {
            Log("gpu: cannot create a %ux%u texture of format %u; it reads as zero", layout.width, layout.height,
                layout.format);
            return nullptr;
        }
        return (textures_[{ upload.address, upload.layoutKey }] = std::move(texture)).get();
    }

    void Textures::Upload(const TextureUpload& upload)
    {
        const textures::Layout& layout = upload.layout;
        const textures::Format* format = textures::FindFormat(layout.format);
        if (format == nullptr)
            return;
        // The host needs block-compressed textures whose base is a multiple of the block.
        if (format->HostCompressed() && (layout.width % format->block != 0 || layout.height % format->block != 0))
            format = textures::FindFormat(layout.format, true);
        if (format == nullptr)
            return;

        auto it = textures_.find({ upload.address, upload.layoutKey });
        Texture* texture = it != textures_.end() ? it->second.get() : Create(upload, *format);
        if (texture == nullptr)
            return;
        changes_++;

        plume::RenderCommandList& list = context_.Commands();
        list.barriers(plume::RenderBarrierStage::COPY,
            plume::RenderTextureBarrier(texture->texture.get(), plume::RenderTextureLayout::COPY_DEST));
        texture->readable = false;
        std::vector<uint8_t> data;
        for (uint32_t face = 0; face < layout.faces; face++)
        {
            for (uint32_t level = layout.hasBase ? 0 : 1; level <= layout.mipMax; level++)
            {
                const textures::LevelPlace& place = layout.levels[level];
                uint32_t width = std::max(layout.width >> level, 1u);
                uint32_t height = std::max(layout.height >> level, 1u);
                textures::GuestLevel guest;
                guest.memory = FindMemory(upload, place.address + face * place.faceStride, guest.bytes);
                guest.tiled = layout.tiled;
                guest.rowPitch = place.rowPitch;
                guest.endian = layout.endian;
                guest.x0 = place.x0;
                guest.y0 = place.y0;
                guest.widthBlocks = (width + format->block - 1) / format->block;
                guest.heightBlocks = (height + format->block - 1) / format->block;
                guest.bytesPerBlock = format->bytesPerBlock;
                if (guest.memory == nullptr || !textures::ReadLevel(guest, data))
                {
                    LOG_ONCE("gpu: a texture level lies outside its memory; it is left as it was");
                    continue;
                }
                textures::ConvertLevel(*format, data, width, height);

                // Host rows: texels when decompressed, otherwise blocks.
                uint32_t rows = format->Decompresses() ? height : guest.heightBlocks;
                uint32_t rowBytes = (format->Decompresses() ? width : guest.widthBlocks) * format->hostBytes;
                constexpr uint32_t kRowAlignment = Context::kTextureRowAlignment;
                uint32_t pitch = (rowBytes + kRowAlignment - 1) / kRowAlignment * kRowAlignment;
                Context::Upload staging = context_.Allocate(uint64_t(pitch) * rows, Context::kTexturePlacementAlignment);
                for (uint32_t row = 0; row < rows; row++)
                    memcpy(staging.data + uint64_t(row) * pitch, data.data() + size_t(row) * rowBytes, rowBytes);
                uint32_t rowWidth = pitch / format->hostBytes * (format->HostCompressed() ? format->block : 1);
                list.copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(texture->texture.get(), level, face),
                    plume::RenderTextureCopyLocation::PlacedFootprint(staging.buffer, format->host, width, height, 1,
                        rowWidth, staging.offset));
            }
        }
    }

    void Textures::Release(uint32_t begin, uint32_t end)
    {
        auto it = textures_.lower_bound({ begin, 0 });
        while (it != textures_.end() && it->first.first < end)
        {
            context_.Retire(std::shared_ptr<Texture>(std::move(it->second)));
            it = textures_.erase(it);
            changes_++;
        }
    }

    Textures::Textures(Context& context, Targets& targets, Descriptors& descriptors, uint32_t anisotropy)
        : context_(context), targets_(targets), descriptors_(descriptors)
    {
        // 1:1 is 1, each doubling one more.
        for (uint32_t ratio = anisotropy; ratio != 0; ratio >>= 1)
            anisotropyField_++;
    }

    uint64_t Textures::Changes() const
    {
        // Neither count goes back, so the sum moves when either does.
        return changes_ + targets_.ResolvedChanges();
    }

    TextureBinding Textures::Bind(plume::RenderCommandList& list, const DrawTexture& texture)
    {
        TextureBinding binding;
        if (texture.kind == DrawTexture::Kind::None)
            return binding;
        // The filter override: on the title's own textures, where the fetch is trilinear (a minifying
        // linear filter between linear levels: the surfaces the anisotropic filter sharpens at a
        // grazing angle). Resolved textures, point fetches and the bone matrices keep the title's.
        SamplerState sampler = texture.sampler;
        uint32_t minFilter = (sampler.filter >> SamplerState::kMinFilterShift) & 3;
        uint32_t mipFilter = (sampler.filter >> SamplerState::kMipFilterShift) & 3;
        if (anisotropyField_ != 0 && !texture.resolved && minFilter == SamplerState::kLinearFilter &&
            mipFilter == SamplerState::kLinearFilter)
        {
            sampler.filter &= ~(7u << SamplerState::kAnisotropyShift);
            sampler.filter |= anisotropyField_ << SamplerState::kAnisotropyShift;
        }
        binding.sampler = descriptors_.Sampler(sampler);

        if (texture.resolved)
        {
            ResolvedTexture* resolved = targets_.Resolved(texture.address);
            if (resolved == nullptr)
                return binding;
            if (texture.kind != DrawTexture::Kind::Planar)
            {
                LOG_ONCE("gpu: a resolved texture sampled as a cube reads as zero");
                return binding;
            }
            Targets::MakeReadable(list, *resolved);
            // A resolved color texture holds the surface's colors in order, whatever swizzle reads them
            // back (k_5_6_5 has no alpha: it reads as one); k_8 and the depth resolves read their
            // values through the format's host swizzle (every component takes red).
            if (texture.format == textures::k8_8_8_8)
            {
                binding.texture2D = resolved->descriptor.Index();
                return binding;
            }
            const textures::Format* format = textures::FindFormat(texture.format);
            plume::RenderTextureViewDesc view = plume::RenderTextureViewDesc::Texture2D(resolved->format);
            if (texture.format == textures::k5_6_5)
                view.componentMapping = { plume::RenderSwizzle::R, plume::RenderSwizzle::G, plume::RenderSwizzle::B,
                    plume::RenderSwizzle::ONE };
            else
                view.componentMapping = Mapping(texture.swizzle, format != nullptr ? format->swizzle : kIdentity);
            binding.texture2D = resolved->views.Index(descriptors_, TextureSet::k2D, *resolved->texture, view);
            return binding;
        }

        auto it = textures_.find({ texture.address, texture.layout });
        if (it == textures_.end())
            return binding;
        Texture& host = *it->second;
        if (!host.readable)
        {
            list.barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(host.texture.get(), plume::RenderTextureLayout::SHADER_READ));
            host.readable = true;
        }
        // The view holds the levels the fetch samples, so the sampler's LOD 0 is the first of them.
        plume::RenderTextureViewDesc view = host.cube ? plume::RenderTextureViewDesc::TextureCube(host.format->host)
                                                      : plume::RenderTextureViewDesc::Texture2D(host.format->host);
        view.mipSlice = texture.mipMin;
        view.mipLevels = texture.mipMax - texture.mipMin + 1;
        view.componentMapping = Mapping(texture.swizzle, host.format->swizzle);
        if (host.cube)
            binding.textureCube = host.views.Index(descriptors_, TextureSet::kCube, *host.texture, view);
        else
            binding.texture2D = host.views.Index(descriptors_, TextureSet::k2D, *host.texture, view);
        return binding;
    }
}
