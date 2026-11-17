// The bindless descriptor sets of the translated shaders and the samplers of the fetch constants.

#include "gpu/renderer/descriptors.h"

#include "gpu/renderer/backend.h"
#include "gpu/renderer/context.h"
#include "log.h"

#include <cstring>

namespace gpu
{
    namespace
    {
        // The format of the zero textures: every component reads as zero, alpha included.
        constexpr plume::RenderFormat kZeroFormat = plume::RenderFormat::R8G8B8A8_UNORM;
        constexpr uint32_t kZeroTexelBytes = 4;
        constexpr uint32_t kCubeFaces = 6;

        plume::RenderTextureAddressMode AddressMode(uint32_t clamp)
        {
            // The halfway clamps take the plain clamp of the same mirroring; the host has no
            // halfway mode.
            switch (clamp)
            {
            case 0: return plume::RenderTextureAddressMode::WRAP;
            case 1: return plume::RenderTextureAddressMode::MIRROR;
            case 2: return plume::RenderTextureAddressMode::CLAMP;
            case 3: return plume::RenderTextureAddressMode::MIRROR_ONCE;
            case 4: return plume::RenderTextureAddressMode::CLAMP;
            case 5: return plume::RenderTextureAddressMode::MIRROR_ONCE;
            case 6: return plume::RenderTextureAddressMode::BORDER;
            default: return plume::RenderTextureAddressMode::MIRROR_ONCE;
            }
        }

        plume::RenderBorderColor BorderColor(uint32_t border)
        {
            // 0: transparent black, 1: opaque white, 2 and 3: the opaque black of the YCbCr layouts.
            switch (border)
            {
            case 0: return plume::RenderBorderColor::TRANSPARENT_BLACK;
            case 1: return plume::RenderBorderColor::OPAQUE_WHITE;
            default: return plume::RenderBorderColor::OPAQUE_BLACK;
            }
        }

        uint64_t SamplerKey(const SamplerState& state)
        {
            return uint64_t(state.clamp & 0x1FF) | uint64_t(state.filter & 0x1FF) << 9 |
                uint64_t(uint32_t(state.lodBias) & 0x3FF) << 18 | uint64_t(state.border & 3) << 28;
        }

        uint64_t ViewKey(const plume::RenderTextureViewDesc& view)
        {
            const plume::RenderComponentMapping& mapping = view.componentMapping;
            return uint64_t(mapping.r) | uint64_t(mapping.g) << 8 | uint64_t(mapping.b) << 16 | uint64_t(mapping.a) << 24 |
                uint64_t(view.mipSlice & 0xFF) << 32 | uint64_t(view.mipLevels & 0xFF) << 40;
        }
    }

    Descriptors::Descriptors(Context& context) : device_(context.Device())
    {
        CreateZeroTextures(context);
        for (uint32_t set = 0; set < kSetCount; set++)
        {
            plume::RenderDescriptorRangeType type =
                set == kSamplerSet ? plume::RenderDescriptorRangeType::SAMPLER : plume::RenderDescriptorRangeType::TEXTURE;
            // The count of a boundless range is its upper bound: Vulkan sizes the binding by it.
            ranges_[set] = plume::RenderDescriptorRange(type, 0, kCapacity[set]);
            setLayouts_[set] = plume::RenderDescriptorSetDesc(&ranges_[set], 1, true, kCapacity[set]);
            sets_[set] = device_.createDescriptorSet(setLayouts_[set]);
            if (set != kSamplerSet)
                SetNull(TextureSet(set), kNullTexture);
        }
        // Sampler 0 for the slots without a texture.
        Sampler({});
    }

    void Descriptors::CreateZeroTextures(Context& context)
    {
        plume::RenderTextureDesc descs[kSetCount - 1] = {
            plume::RenderTextureDesc::Texture2D(1, 1, 1, kZeroFormat),
            plume::RenderTextureDesc::Texture3D(1, 1, 1, 1, kZeroFormat),
            plume::RenderTextureDesc::Texture2D(1, 1, 1, kZeroFormat, plume::RenderTextureFlag::CUBE),
        };
        descs[uint32_t(TextureSet::kCube)].arraySize = kCubeFaces;
        const plume::RenderTextureViewDesc views[kSetCount - 1] = {
            plume::RenderTextureViewDesc::Texture2D(kZeroFormat),
            plume::RenderTextureViewDesc::Texture3D(kZeroFormat),
            plume::RenderTextureViewDesc::TextureCube(kZeroFormat),
        };

        plume::RenderCommandList& list = context.Commands();
        for (uint32_t set = 0; set < kSetCount - 1; set++)
        {
            zeroTextures_[set] = device_.createTexture(descs[set]);
            if (!IsCreated(zeroTextures_[set].get()))
                Fatal("gpu: cannot create the zero texture of texture set %u", set);
            zeroViews_[set] = zeroTextures_[set]->createTextureView(views[set]);

            plume::RenderTexture* texture = zeroTextures_[set].get();
            list.barriers(plume::RenderBarrierStage::COPY,
                plume::RenderTextureBarrier(texture, plume::RenderTextureLayout::COPY_DEST));
            uint32_t faces = set == uint32_t(TextureSet::kCube) ? kCubeFaces : 1;
            for (uint32_t face = 0; face < faces; face++)
            {
                // One row of one texel.
                Context::Upload zeros = context.Allocate(Context::kTextureRowAlignment, Context::kTexturePlacementAlignment);
                memset(zeros.data, 0, Context::kTextureRowAlignment);
                list.copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(texture, 0, face),
                    plume::RenderTextureCopyLocation::PlacedFootprint(zeros.buffer, kZeroFormat, 1, 1, 1,
                        Context::kTextureRowAlignment / kZeroTexelBytes, zeros.offset));
            }
            list.barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(texture, plume::RenderTextureLayout::SHADER_READ));
        }
    }

    void Descriptors::Bind(plume::RenderCommandList& list) const
    {
        for (uint32_t set = 0; set < kSetCount; set++)
            list.setGraphicsDescriptorSet(sets_[set].get(), set);
    }

    void Descriptors::SetNull(TextureSet set, uint32_t index)
    {
        uint32_t s = uint32_t(set);
        sets_[s]->setTexture(index, zeroTextures_[s].get(), plume::RenderTextureLayout::SHADER_READ, zeroViews_[s].get());
    }

    uint32_t Descriptors::AddTexture(TextureSet set, const plume::RenderTexture& texture, const plume::RenderTextureView* view)
    {
        uint32_t s = uint32_t(set);
        uint32_t index;
        if (!freeTextures_[s].empty())
        {
            index = freeTextures_[s].back();
            freeTextures_[s].pop_back();
        }
        else
        {
            if (nextTexture_[s] == kCapacity[s])
                Fatal("gpu: more than %u texture descriptors in set %u", kCapacity[s], s);
            index = nextTexture_[s]++;
        }
        sets_[s]->setTexture(index, &texture, plume::RenderTextureLayout::SHADER_READ, view);
        return index;
    }

    void Descriptors::ReleaseTexture(TextureSet set, uint32_t index)
    {
        SetNull(set, index);
        freeTextures_[uint32_t(set)].push_back(index);
    }

    uint32_t Descriptors::Sampler(const SamplerState& state)
    {
        uint64_t key = SamplerKey(state);
        auto it = samplerIndices_.find(key);
        if (it != samplerIndices_.end())
            return it->second;

        uint32_t index = uint32_t(samplers_.size());
        if (index == kCapacity[kSamplerSet])
        {
            LOG_ONCE("gpu: more than %u samplers; the others sample with sampler 0", kCapacity[kSamplerSet]);
            return 0;
        }
        // Filters: mag, min and mip (2 bits each; 1 is linear) and the anisotropic filter.
        uint32_t filter = state.filter;
        plume::RenderSamplerDesc desc;
        desc.addressU = AddressMode(state.clamp & 7);
        desc.addressV = AddressMode((state.clamp >> 3) & 7);
        desc.addressW = AddressMode((state.clamp >> 6) & 7);
        desc.magFilter = (filter & 3) == 1 ? plume::RenderFilter::LINEAR : plume::RenderFilter::NEAREST;
        desc.minFilter = ((filter >> 2) & 3) == 1 ? plume::RenderFilter::LINEAR : plume::RenderFilter::NEAREST;
        desc.mipmapMode = ((filter >> 4) & 3) == 1 ? plume::RenderMipmapMode::LINEAR : plume::RenderMipmapMode::NEAREST;
        // Anisotropic filter: 0 off, 1 to 5 for 1:1 up to 16:1.
        uint32_t aniso = (filter >> 6) & 7;
        if (aniso >= 1 && aniso <= 5)
        {
            desc.anisotropyEnabled = true;
            desc.maxAnisotropy = 1u << (aniso - 1);
        }
        // Mip filter 2 samples the first level of the view only.
        if (((filter >> 4) & 3) == 2)
            desc.maxLOD = 0.0f;
        desc.mipLODBias = float(state.lodBias) / 32.0f;
        desc.borderColor = BorderColor(state.border);
        samplers_.push_back(device_.createSampler(desc));
        sets_[kSamplerSet]->setSampler(index, samplers_.back().get());
        samplerIndices_.emplace(key, index);
        return index;
    }

    uint32_t TextureViews::Index(Descriptors& descriptors, TextureSet set, const plume::RenderTexture& texture,
        const plume::RenderTextureViewDesc& desc)
    {
        View& view = views_[ViewKey(desc)];
        if (!view.view)
        {
            view.view = texture.createTextureView(desc);
            view.descriptor = TextureDescriptor(descriptors, set, texture, view.view.get());
        }
        return view.descriptor.Index();
    }
}
