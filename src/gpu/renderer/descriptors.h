#pragma once

#include "gpu/renderer/renderer.h"

#include <plume_render_interface.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace gpu
{
    // The texture sets of the translated shaders, in register space order. 1D textures go in the
    // 2D set (the shaders sample them as 2D textures one texel high).
    enum class TextureSet : uint32_t
    {
        k2D = 0,
        k3D = 1,
        kCube = 2,
    };

    // The bindless descriptor sets of the translated shaders: the 2D, 3D and cube texture heaps and
    // the sampler heap (register spaces 0 to 3), indexed through the shared constants. Index 0 of
    // every texture heap is a null texture of the heap's dimension, which reads as zero. Owner
    // thread only.
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

        // A texture descriptor (view null: the texture's default view). Release it only once no
        // recorded work reads it.
        uint32_t AddTexture(TextureSet set, const plume::RenderTexture& texture, const plume::RenderTextureView* view);
        void ReleaseTexture(TextureSet set, uint32_t index);

        // The sampler of a fetch constant's sampler state, created on first use.
        uint32_t Sampler(const SamplerState& state);

    private:
        void SetNull(TextureSet set, uint32_t index);

        plume::RenderDevice& device_;
        plume::RenderDescriptorRange ranges_[kSetCount];
        plume::RenderDescriptorSetDesc setLayouts_[kSetCount];
        std::unique_ptr<plume::RenderDescriptorSet> sets_[kSetCount];
        std::vector<uint32_t> freeTextures_[kSetCount - 1];
        uint32_t nextTexture_[kSetCount - 1] = { kNullTexture + 1, kNullTexture + 1, kNullTexture + 1 };
        std::unordered_map<uint64_t, uint32_t> samplerIndices_; // by the packed sampler state
        std::vector<std::unique_ptr<plume::RenderSampler>> samplers_;
    };

    // A texture descriptor released when the holder goes away (with the object retired through
    // Context::Retire, after the work that used it).
    class TextureDescriptor
    {
    public:
        TextureDescriptor() = default;
        TextureDescriptor(Descriptors& descriptors, TextureSet set, const plume::RenderTexture& texture,
            const plume::RenderTextureView* view = nullptr)
            : descriptors_(&descriptors), set_(set), index_(descriptors.AddTexture(set, texture, view)) {}
        TextureDescriptor(TextureDescriptor&& other) noexcept { *this = std::move(other); }
        TextureDescriptor& operator=(TextureDescriptor&& other) noexcept
        {
            Reset();
            descriptors_ = other.descriptors_;
            set_ = other.set_;
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
                descriptors_->ReleaseTexture(set_, index_);
            descriptors_ = nullptr;
        }

        Descriptors* descriptors_ = nullptr;
        TextureSet set_ = TextureSet::k2D;
        uint32_t index_ = Descriptors::kNullTexture;
    };

    // The shader views of one texture by component mapping and level range, each with its descriptor.
    // They go away with the holder, which retires them with its texture.
    class TextureViews
    {
    public:
        // The descriptor of a view (format, dimension, levels, mapping), created on first use.
        uint32_t Index(Descriptors& descriptors, TextureSet set, const plume::RenderTexture& texture,
            const plume::RenderTextureViewDesc& desc);

    private:
        struct View
        {
            std::unique_ptr<plume::RenderTextureView> view;
            TextureDescriptor descriptor; // released before the view
        };
        std::unordered_map<uint64_t, View> views_;
    };
}
