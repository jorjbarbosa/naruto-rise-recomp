// The bindless descriptor sets of the translated shaders and the samplers of the fetch constants.

#include "gpu/renderer/descriptors.h"

#include "log.h"

namespace gpu
{
    namespace
    {
        // Capacities of the sets: 2D textures, 3D textures, cube textures, samplers.
        constexpr uint32_t kCapacity[Descriptors::kSetCount] = { 16384, 1024, 1024, 512 };
        constexpr uint32_t kSamplerSet = 3;

        // Texture fetch constant fields of the sampler: dword 0 holds the clamp modes of X, Y and Z
        // (3 bits each from bit 10), dword 3 the mag, min and mip filters (2 bits each from bit 19)
        // and the anisotropic filter (3 bits from bit 25).
        constexpr uint32_t kClampShift = 10;
        constexpr uint32_t kClampMask = 0x1FF;
        constexpr uint32_t kFilterShift = 19;
        constexpr uint32_t kFilterMask = 0x1FF;

        plume::RenderTextureAddressMode AddressMode(uint32_t clamp)
        {
            // The halfway and border clamps reach the border color; the host has no halfway mode.
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
    }

    Descriptors::Descriptors(plume::RenderDevice& device) : device_(device)
    {
        for (uint32_t set = 0; set < kSetCount; set++)
        {
            plume::RenderDescriptorRangeType type =
                set == kSamplerSet ? plume::RenderDescriptorRangeType::SAMPLER : plume::RenderDescriptorRangeType::TEXTURE;
            ranges_[set] = plume::RenderDescriptorRange(type, 0, 1);
            setLayouts_[set] = plume::RenderDescriptorSetDesc(&ranges_[set], 1, true, kCapacity[set]);
            sets_[set] = device.createDescriptorSet(setLayouts_[set]);
            if (set != kSamplerSet)
                sets_[set]->setTexture(kNullTexture, nullptr, plume::RenderTextureLayout::SHADER_READ);
        }
        // Sampler 0 for the slots without a texture.
        Sampler(0, 0);
    }

    void Descriptors::Bind(plume::RenderCommandList& list) const
    {
        for (uint32_t set = 0; set < kSetCount; set++)
            list.setGraphicsDescriptorSet(sets_[set].get(), set);
    }

    uint32_t Descriptors::AddTexture2D(const plume::RenderTexture& texture)
    {
        uint32_t index;
        if (!freeTextures2D_.empty())
        {
            index = freeTextures2D_.back();
            freeTextures2D_.pop_back();
        }
        else
        {
            if (nextTexture2D_ == kCapacity[0])
                Fatal("gpu: more than %u 2D texture descriptors", kCapacity[0]);
            index = nextTexture2D_++;
        }
        sets_[0]->setTexture(index, &texture, plume::RenderTextureLayout::SHADER_READ);
        return index;
    }

    void Descriptors::ReleaseTexture2D(uint32_t index)
    {
        sets_[0]->setTexture(index, nullptr, plume::RenderTextureLayout::SHADER_READ);
        freeTextures2D_.push_back(index);
    }

    uint32_t Descriptors::Sampler(uint32_t fetch0, uint32_t fetch3)
    {
        uint32_t clamp = (fetch0 >> kClampShift) & kClampMask;
        uint32_t filter = (fetch3 >> kFilterShift) & kFilterMask;
        uint32_t key = clamp | filter << 9;
        auto it = samplerIndices_.find(key);
        if (it != samplerIndices_.end())
            return it->second;

        uint32_t index = uint32_t(samplers_.size());
        if (index == kCapacity[kSamplerSet])
            Fatal("gpu: more than %u samplers", kCapacity[kSamplerSet]);
        plume::RenderSamplerDesc desc;
        desc.addressU = AddressMode(clamp & 7);
        desc.addressV = AddressMode((clamp >> 3) & 7);
        desc.addressW = AddressMode((clamp >> 6) & 7);
        desc.magFilter = (filter & 3) == 1 ? plume::RenderFilter::LINEAR : plume::RenderFilter::NEAREST;
        desc.minFilter = ((filter >> 2) & 3) == 1 ? plume::RenderFilter::LINEAR : plume::RenderFilter::NEAREST;
        // Mip filter 2 is the base map only.
        uint32_t mip = (filter >> 4) & 3;
        desc.mipmapMode = mip == 1 ? plume::RenderMipmapMode::LINEAR : plume::RenderMipmapMode::NEAREST;
        if (mip == 2)
            desc.maxLOD = 0.0f;
        // Anisotropic filter: 0 off, 1 to 5 for 1:1 up to 16:1.
        uint32_t aniso = (filter >> 6) & 7;
        if (aniso >= 1 && aniso <= 5)
        {
            desc.anisotropyEnabled = true;
            desc.maxAnisotropy = 1u << (aniso - 1);
        }
        desc.borderColor = plume::RenderBorderColor::TRANSPARENT_BLACK;
        samplers_.push_back(device_.createSampler(desc));
        sets_[kSamplerSet]->setSampler(index, samplers_.back().get());
        samplerIndices_.emplace(key, index);
        return index;
    }
}
