#pragma once

#include "gpu/renderer/descriptors.h"
#include "gpu/renderer/renderer.h"

#include <plume_render_interface.h>

#include <cstdint>
#include <map>
#include <memory>
#include <utility>

namespace gpu
{
    class Context;
    class Targets;
    namespace textures
    {
        struct Format;
    }

    // The host copies of the guest textures the draws sample, by (GPU address, layout key), filled
    // from the uploads the guest side posts when their memory changes. Owner thread only.
    class Textures
    {
    public:
        Textures(Context& context, Targets& targets, Descriptors& descriptors)
            : context_(context), targets_(targets), descriptors_(descriptors) {}

        // Creates the texture on first use and records the copy of every level and face.
        void Upload(const TextureUpload& upload);
        // Releases the textures uploaded from an address, once the GPU no longer uses them.
        void Release(uint32_t address);
        // Sets the descriptor indices of a draw's fetch slot, recording the barrier that makes its
        // texture readable. A texture that is missing or not covered leaves the slot reading zero.
        void Bind(plume::RenderCommandList& list, uint32_t slot, const DrawTexture& texture, SharedConstants& shared);

    private:
        struct Texture
        {
            const textures::Format* format = nullptr;
            bool cube = false;
            std::unique_ptr<plume::RenderTexture> texture;
            TextureViews views; // released before the texture
        };
        using Key = std::pair<uint32_t, uint64_t>;

        Texture* Create(const TextureUpload& upload, const textures::Format& format);

        Context& context_;
        Targets& targets_;
        Descriptors& descriptors_;
        std::map<Key, std::unique_ptr<Texture>> textures_; // ordered by address, for Release
    };
}
