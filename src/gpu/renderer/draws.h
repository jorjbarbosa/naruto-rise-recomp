#pragma once

#include "gpu/renderer/pipelines.h"
#include "gpu/renderer/renderer.h"

#include <plume_render_interface.h>

#include <filesystem>
#include <memory>

namespace gpu
{
    class Buffers;
    class Context;
    class Descriptors;
    class Targets;
    class Textures;

    // The guest draws: the recording of each draw with its own copies of the indices and constants,
    // and of the vertices the host keeps no copy of (Buffers), over the pipelines of Pipelines. Owner
    // thread only.
    class Draws
    {
    public:
        // pipelineList: the file the pipelines are remembered in (Pipelines; empty: none).
        Draws(Context& context, Targets& targets, Descriptors& descriptors, Textures& textures, Buffers& buffers,
            const std::filesystem::path& pipelineList);
        ~Draws();

        // Records the draw; a draw whose shaders or pipeline are missing is skipped (logged).
        void Draw(DrawOp& op);

    private:
        Context& context_;
        Targets& targets_;
        Descriptors& descriptors_;
        Textures& textures_;
        Buffers& buffers_;
        std::unique_ptr<plume::RenderPipelineLayout> layout_;
        std::unique_ptr<Pipelines> pipelines_; // after layout_, which it uses
    };
}
