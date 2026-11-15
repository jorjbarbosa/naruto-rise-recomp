#pragma once

#include <plume_render_interface.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace gpu
{
    // The bindless descriptor sets of the translated shaders: the 2D, 3D and cube texture heaps and
    // the sampler heap (register spaces 0 to 3), indexed through the shared constants. Index 0 of
    // every texture heap is a null texture, which reads as zero. Owner thread only.
    class Descriptors
    {
    public:
        static constexpr uint32_t kNullTexture = 0;
        static constexpr uint32_t kSetCount = 4;

        explicit Descriptors(plume::RenderDevice& device);

        // The layouts of the four sets, for the pipeline layout.
        const plume::RenderDescriptorSetDesc* SetLayouts() const { return setLayouts_; }
        // Binds the four sets (the pipeline layout must be set).
        void Bind(plume::RenderCommandList& list) const;

        // A 2D texture descriptor. Release it only once no recorded work reads it.
        uint32_t AddTexture2D(const plume::RenderTexture& texture);
        void ReleaseTexture2D(uint32_t index);

        // The sampler of a texture fetch constant (dwords 0 and 3, host order), created on first use.
        uint32_t Sampler(uint32_t fetch0, uint32_t fetch3);

    private:
        plume::RenderDevice& device_;
        plume::RenderDescriptorRange ranges_[kSetCount];
        plume::RenderDescriptorSetDesc setLayouts_[kSetCount];
        std::unique_ptr<plume::RenderDescriptorSet> sets_[kSetCount];
        std::vector<uint32_t> freeTextures2D_;
        uint32_t nextTexture2D_ = kNullTexture + 1;
        std::unordered_map<uint32_t, uint32_t> samplerIndices_; // by the fetch fields of the sampler
        std::vector<std::unique_ptr<plume::RenderSampler>> samplers_;
    };

    // A 2D texture descriptor released when the holder goes away (with the object retired through
    // Context::Retire, after the work that used it).
    class TextureDescriptor
    {
    public:
        TextureDescriptor() = default;
        TextureDescriptor(Descriptors& descriptors, const plume::RenderTexture& texture)
            : descriptors_(&descriptors), index_(descriptors.AddTexture2D(texture)) {}
        TextureDescriptor(TextureDescriptor&& other) noexcept { *this = std::move(other); }
        TextureDescriptor& operator=(TextureDescriptor&& other) noexcept
        {
            Reset();
            descriptors_ = other.descriptors_;
            index_ = other.index_;
            other.descriptors_ = nullptr;
            return *this;
        }
        ~TextureDescriptor() { Reset(); }

        uint32_t Index() const { return descriptors_ != nullptr ? index_ : Descriptors::kNullTexture; }

    private:
        void Reset()
        {
            if (descriptors_ != nullptr)
                descriptors_->ReleaseTexture2D(index_);
            descriptors_ = nullptr;
        }

        Descriptors* descriptors_ = nullptr;
        uint32_t index_ = Descriptors::kNullTexture;
    };
}
