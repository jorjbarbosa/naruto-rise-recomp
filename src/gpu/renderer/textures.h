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

    // The descriptor indices a draw's shaders read a fetch slot through; the defaults read as zero.
    struct TextureBinding
    {
        uint32_t texture2D = Descriptors::kNullTexture;
        uint32_t textureCube = Descriptors::kNullTexture;
        uint32_t sampler = 0;
    };

    // The host copies of the guest textures the draws sample, by (GPU address, layout key), filled
    // from the uploads the guest side posts when their memory changes. Owner thread only.
    class Textures
    {
    public:
        Textures(Context& context, Targets& targets, Descriptors& descriptors)
            : context_(context), targets_(targets), descriptors_(descriptors) {}

        // Creates the texture on first use and records the copy of every level and face.
        void Upload(const TextureUpload& upload);
        // Releases the textures uploaded from the GPU addresses [begin, end), once the GPU no longer
        // uses them.
        void Release(uint32_t begin, uint32_t end);
        // The descriptor indices of a fetch slot's texture, recording the barrier that makes it
        // readable. A texture that is missing or not covered reads as zero. The answer and the barrier
        // hold for the draws that follow in the command list while Changes() stays the same.
        TextureBinding Bind(plume::RenderCommandList& list, const DrawTexture& texture);
        // A number that moves whenever Bind may answer differently for the same texture or has to
        // record its barrier again: an upload, a release, or a change to the resolved textures.
        uint64_t Changes() const;

    private:
        struct Texture
        {
            const textures::Format* format = nullptr;
            bool cube = false;
            std::unique_ptr<plume::RenderTexture> texture;
            TextureViews views; // released before the texture
            // In SHADER_READ with no upload since: the draws that sample it need no barrier.
            bool readable = false;
        };
        using Key = std::pair<uint32_t, uint64_t>;

        Texture* Create(const TextureUpload& upload, const textures::Format& format);

        Context& context_;
        Targets& targets_;
        Descriptors& descriptors_;
        std::map<Key, std::unique_ptr<Texture>> textures_; // ordered by address, for Release
        uint64_t changes_ = 0; // uploads and releases
    };
}
