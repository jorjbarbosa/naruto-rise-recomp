#pragma once

#include "gpu/renderer/renderer.h"
#include "gpu/renderer/shader_cache.h"

#include <plume_render_interface.h>

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace gpu
{
    class Context;
    class Descriptors;
    class Targets;
    class Textures;

    // The guest draws: pipelines over the translated shaders, created on first use, and the
    // recording of each draw with its own copies of the vertices, indices and constants. Owner
    // thread only.
    class Draws
    {
    public:
        Draws(Context& context, Targets& targets, Descriptors& descriptors, Textures& textures);
        ~Draws();

        // Records the draw; a draw whose shaders or pipeline are missing is skipped (logged).
        void Draw(DrawOp& op);

    private:
        // Everything a pipeline depends on. Plain data, hashed and compared as bytes.
        struct PipelineKey
        {
            uint64_t vertexShader = 0;
            uint64_t pixelShader = 0;
            uint64_t input = 0;
            RenderState state;
            uint32_t samples = 0;
            uint32_t targets = 0; // bit 0: color, bit 1: depth-stencil

            bool operator==(const PipelineKey&) const = default;
        };
        struct PipelineKeyHash
        {
            size_t operator()(const PipelineKey& key) const;
        };

        // The shader of a cache key, or null when the cache has none (logged once per key).
        const plume::RenderShader* Shader(uint64_t key);
        // The pipeline of a draw, or null when it cannot be created (logged once per key).
        const plume::RenderPipeline* Pipeline(const DrawOp& op);

        Context& context_;
        Targets& targets_;
        Descriptors& descriptors_;
        Textures& textures_;
        ShaderCache cache_;
        std::unique_ptr<plume::RenderPipelineLayout> layout_;
        std::unordered_map<uint64_t, std::unique_ptr<plume::RenderShader>> shaders_;
        std::unordered_map<PipelineKey, std::unique_ptr<plume::RenderPipeline>, PipelineKeyHash> pipelines_;
    };
}
