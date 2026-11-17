#pragma once

#include "gpu/renderer/backend.h"
#include "gpu/renderer/context.h"
#include "gpu/renderer/pipelines.h"
#include "gpu/renderer/renderer.h"
#include "gpu/renderer/textures.h"

#include <plume_render_interface.h>

#include <array>
#include <filesystem>
#include <memory>
#include <optional>

namespace gpu
{
    class Buffers;
    class ConstantRing;
    class Descriptors;
    class Targets;

    // The vertex buffer views of a draw, one per stream.
    struct VertexStreams
    {
        uint32_t count = 0;
        std::array<plume::RenderVertexBufferView, kMaxVertexStreams> views;
        std::array<plume::RenderInputSlot, kMaxVertexStreams> slots;
    };

    // The guest draws: the recording of each draw over the pipelines of Pipelines, with the vertices
    // and indices from the host copies of their buffers (Buffers) or from the draw's own copies, and
    // its constants copied from the constant rings once for the draws in a row that share a range.
    // State the previous draw left in the command list is not set again, and a fetch slot is bound
    // again only when its texture differs from the previous draw's. Owner thread only.
    class Draws
    {
    public:
        // pipelineList: the file the pipelines are remembered in (Pipelines; empty: none).
        Draws(Context& context, Targets& targets, Descriptors& descriptors, Textures& textures, Buffers& buffers,
            const ConstantRing& vertexRing, const ConstantRing& pixelRing, const std::filesystem::path& pipelineList);
        ~Draws();

        // Records the draw; a draw whose shaders or pipeline are missing is skipped (logged).
        void Draw(DrawOp& op);

    private:
        // The constants of a stage uploaded for a draw of the open command list, which the draws after
        // it with the same range reuse.
        struct ConstantUpload
        {
            uint64_t position = UINT64_MAX; // of the range; none yet
            Context::Upload upload;
            uint64_t submission = 0;
        };

        // The upload of a draw's constants: the last one when the range is the same and the upload is
        // still in the open command list, otherwise a new copy from the ring.
        plume::RenderBufferReference Constants(ConstantUpload& last, const ConstantRing& ring, const ConstantRange& range);

        // The shared constants of the last draw of the open command list.
        struct SharedUpload
        {
            SharedConstants values;
            Context::Upload upload;
            uint64_t submission = 0;
        };

        // The upload of a draw's shared constants: the last one when they are the same and still in the
        // open command list, otherwise a new copy.
        plume::RenderBufferReference Shared(const SharedConstants& shared);

        // The state the last draw left in the open command list while Draws held its graphics state
        // (Context::ClaimGraphicsState), which the next draws set again only where it differs; an
        // empty field is set by the next draw.
        struct ListState
        {
            bool layoutSet = false; // the pipeline layout and the descriptor sets
            const plume::RenderPipeline* pipeline = nullptr;
            std::optional<DrawConstants> constants;
            std::optional<std::array<float, 4>> blendFactor;
            int32_t stencilReference = -1; // -1: not set
            std::optional<plume::RenderViewport> viewport;
            std::optional<Rect> scissor;
            std::optional<VertexStreams> streams;
            std::optional<plume::RenderIndexBufferView> indices;
            // The fetch slots as the last draw bound them: good for a draw with the same texture in
            // the slot while Textures::Changes() stays at textureChanges.
            std::array<DrawTexture, SharedConstants::kFetchSlots> textures{};
            std::array<TextureBinding, SharedConstants::kFetchSlots> bindings{};
            uint64_t textureChanges = 0;
        };

        Context& context_;
        Targets& targets_;
        Descriptors& descriptors_;
        Textures& textures_;
        Buffers& buffers_;
        const ConstantRing& vertexRing_;
        const ConstantRing& pixelRing_;
        std::unique_ptr<plume::RenderPipelineLayout> layout_;
        std::unique_ptr<Pipelines> pipelines_; // after layout_, which it uses
        ConstantUpload vertexConstants_;
        ConstantUpload pixelConstants_;
        SharedUpload shared_;
        ListState state_;
    };
}
