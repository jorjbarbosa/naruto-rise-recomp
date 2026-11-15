// Guest textures on the host: creation, uploads of their levels and faces, and the descriptors the
// draws sample them through.

#include "gpu/renderer/textures.h"

#include "gpu/renderer/context.h"
#include "gpu/renderer/targets.h"
#include "gpu/textures/formats.h"
#include "log.h"

#include <plume_d3d12.h>

#include <algorithm>
#include <cstring>

namespace gpu
{
    namespace
    {
        // Placed footprints: rows aligned to 256 bytes, subresources to 512.
        constexpr uint32_t kRowAlignment = 256;
        constexpr uint32_t kPlacementAlignment = 512;

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
        if (static_cast<plume::D3D12Texture*>(texture->texture.get())->d3d == nullptr)
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

        plume::RenderCommandList& list = context_.Commands();
        list.barriers(plume::RenderBarrierStage::COPY,
            plume::RenderTextureBarrier(texture->texture.get(), plume::RenderTextureLayout::COPY_DEST));
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
                uint32_t pitch = (rowBytes + kRowAlignment - 1) / kRowAlignment * kRowAlignment;
                Context::Upload staging = context_.Allocate(uint64_t(pitch) * rows, kPlacementAlignment);
                for (uint32_t row = 0; row < rows; row++)
                    memcpy(staging.data + uint64_t(row) * pitch, data.data() + size_t(row) * rowBytes, rowBytes);
                uint32_t rowWidth = pitch / format->hostBytes * (format->HostCompressed() ? format->block : 1);
                list.copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(texture->texture.get(), level, face),
                    plume::RenderTextureCopyLocation::PlacedFootprint(staging.buffer, format->host, width, height, 1,
                        rowWidth, staging.offset));
            }
        }
    }

    void Textures::Release(uint32_t address)
    {
        auto it = textures_.lower_bound({ address, 0 });
        while (it != textures_.end() && it->first.first == address)
        {
            context_.Retire(std::shared_ptr<Texture>(std::move(it->second)));
            it = textures_.erase(it);
        }
    }

    void Textures::Bind(plume::RenderCommandList& list, uint32_t slot, const DrawTexture& texture, SharedConstants& shared)
    {
        if (texture.kind == DrawTexture::Kind::None)
            return;
        shared.sampler[slot] = descriptors_.Sampler(texture.sampler);

        if (texture.resolved)
        {
            ResolvedTexture* resolved = targets_.Resolved(texture.address);
            if (resolved == nullptr)
                return;
            if (texture.kind != DrawTexture::Kind::Planar)
            {
                LOG_ONCE("gpu: a resolved texture sampled as a cube reads as zero");
                return;
            }
            list.barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(resolved->texture.get(), plume::RenderTextureLayout::SHADER_READ));
            // A resolved k_8_8_8_8 holds the surface's colors in order, whatever swizzle reads them
            // back; other formats read the resolved values through their host swizzle.
            if (texture.format == textures::k8_8_8_8)
            {
                shared.texture2D[slot] = resolved->descriptor.Index();
                return;
            }
            const textures::Format* format = textures::FindFormat(texture.format);
            plume::RenderTextureViewDesc view = plume::RenderTextureViewDesc::Texture2D(resolved->format);
            view.componentMapping = Mapping(texture.swizzle, format != nullptr ? format->swizzle : kIdentity);
            shared.texture2D[slot] = resolved->views.Index(descriptors_, TextureSet::k2D, *resolved->texture, view);
            return;
        }

        auto it = textures_.find({ texture.address, texture.layout });
        if (it == textures_.end())
            return;
        Texture& host = *it->second;
        list.barriers(plume::RenderBarrierStage::GRAPHICS,
            plume::RenderTextureBarrier(host.texture.get(), plume::RenderTextureLayout::SHADER_READ));
        // The view holds the levels the fetch samples, so the sampler's LOD 0 is the first of them.
        plume::RenderTextureViewDesc view = host.cube ? plume::RenderTextureViewDesc::TextureCube(host.format->host)
                                                      : plume::RenderTextureViewDesc::Texture2D(host.format->host);
        view.mipSlice = texture.mipMin;
        view.mipLevels = texture.mipMax - texture.mipMin + 1;
        view.componentMapping = Mapping(texture.swizzle, host.format->swizzle);
        if (host.cube)
            shared.textureCube[slot] = host.views.Index(descriptors_, TextureSet::kCube, *host.texture, view);
        else
            shared.texture2D[slot] = host.views.Index(descriptors_, TextureSet::k2D, *host.texture, view);
    }
}
