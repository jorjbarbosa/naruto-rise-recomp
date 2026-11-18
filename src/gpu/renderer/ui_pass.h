#pragma once

#include "gpu/ui_updates.h"

#include <plume_render_interface.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace gpu
{
    class Context;
    class Passes;

    // The host UI's textures and frames on the GPU (gpu/ui.h). Owner thread only.
    //
    // Each present takes what the UI sent with BeginPresent (the creates are recorded, the newest
    // frame kept), draws the frame with Draw, and ends with EndPresent (the releases).
    class UiPass
    {
    public:
        UiPass(Context& context, const Passes& passes, plume::RenderFormat format);
        ~UiPass();

        void BeginPresent();
        void EndPresent();

        // The blur the frame asks for behind it, 0 to 1.
        float Blur() const;
        // Draws the frame over the bound framebuffer, of this size in pixels. Sets the viewport and
        // scissors.
        void Draw(plume::RenderCommandList& list, uint32_t width, uint32_t height);

    private:
        struct Sampler
        {
            std::unique_ptr<plume::RenderSampler> sampler;
            std::unique_ptr<plume::RenderDescriptorSet> set; // released before the sampler
        };

        struct Texture
        {
            std::unique_ptr<plume::RenderTexture> texture;
            std::unique_ptr<plume::RenderDescriptorSet> sampling; // released before the texture
            bool straightAlpha = false;
        };

        // Records the creation and the copy of every level; null when the description is wrong or
        // the texture cannot be made (logged).
        std::unique_ptr<Texture> Create(const ui::TextureDesc& desc);
        void CreateWhite();

        Context& context_;
        const Passes& passes_;
        Sampler clamp_;
        Sampler wrap_;
        std::unique_ptr<plume::RenderPipelineLayout> layout_;
        std::unique_ptr<plume::RenderPipeline> pipeline_;
        std::unique_ptr<Texture> white_; // for the draws with no texture
        std::unordered_map<ui::Texture, std::unique_ptr<Texture>> textures_;
        std::shared_ptr<const ui::Frame> frame_;
        std::vector<bool> repeats_;         // by draw of the frame: the wrap sampler, else the clamp one
        std::vector<ui::Texture> releases_; // taken by BeginPresent, applied by EndPresent once unused
    };
}
