// Guest draws: recording of each draw over its pipeline, with its constants, vertices and indices.

#include "gpu/renderer/draws.h"

#include "gpu/renderer/buffers.h"
#include "gpu/renderer/context.h"
#include "gpu/renderer/descriptors.h"
#include "gpu/renderer/targets.h"
#include "gpu/renderer/textures.h"
#include "log.h"

#include <plume_d3d12.h>

#include <algorithm>
#include <cstring>

namespace gpu
{
    namespace
    {
        // Root constant buffers of the translated shaders (register b0..b2, space 4).
        constexpr uint32_t kConstantSpace = 4;
        constexpr uint32_t kVertexConstantsRoot = 0;
        constexpr uint32_t kPixelConstantsRoot = 1;
        constexpr uint32_t kSharedConstantsRoot = 2;

        // RB_DEPTHCONTROL: stencil test on.
        constexpr uint32_t kStencilEnable = 0x1;

        // The blend color of a draw: its constant color, or its alpha for the constant alpha factors.
        bool UsesBlendColor(uint32_t control, bool& alphaOnly)
        {
            bool color = false;
            bool alpha = false;
            for (uint32_t factor : { control & 0x1F, (control >> 8) & 0x1F, (control >> 16) & 0x1F, (control >> 24) & 0x1F })
            {
                color |= factor == 12 || factor == 13;
                alpha |= factor == 14 || factor == 15;
            }
            if (color && alpha)
                LOG_ONCE("gpu: a blend uses both the constant color and the constant alpha; the color is used");
            alphaOnly = alpha && !color;
            return color || alpha;
        }

        bool Same(const VertexStreams& a, const VertexStreams& b)
        {
            if (a.count != b.count)
                return false;
            for (uint32_t i = 0; i < a.count; i++)
                if (a.views[i].buffer != b.views[i].buffer || a.views[i].size != b.views[i].size ||
                    a.slots[i].stride != b.slots[i].stride)
                    return false;
            return true;
        }

        bool Same(const plume::RenderIndexBufferView& a, const plume::RenderIndexBufferView& b)
        {
            return a.buffer == b.buffer && a.size == b.size && a.format == b.format;
        }

        // Sets a root constant buffer unless it is the one already set.
        void SetRoot(plume::RenderCommandList& list, std::optional<plume::RenderBufferReference>& current,
            const plume::RenderBufferReference& reference, uint32_t index)
        {
            if (current && *current == reference)
                return;
            list.setGraphicsRootDescriptor(reference, index);
            current = reference;
        }
    }

    Draws::Draws(Context& context, Targets& targets, Descriptors& descriptors, Textures& textures, Buffers& buffers,
        const std::filesystem::path& pipelineList)
        : context_(context), targets_(targets), descriptors_(descriptors), textures_(textures), buffers_(buffers)
    {
        const plume::RenderRootDescriptorDesc roots[] = {
            { kVertexConstantsRoot, kConstantSpace, plume::RenderRootDescriptorType::CONSTANT_BUFFER },
            { kPixelConstantsRoot, kConstantSpace, plume::RenderRootDescriptorType::CONSTANT_BUFFER },
            { kSharedConstantsRoot, kConstantSpace, plume::RenderRootDescriptorType::CONSTANT_BUFFER },
        };
        plume::RenderPipelineLayoutDesc desc(nullptr, 0, descriptors.SetLayouts(), Descriptors::kSetCount, false, true);
        desc.rootDescriptorDescs = roots;
        desc.rootDescriptorDescsCount = uint32_t(std::size(roots));
        layout_ = context.Device().createPipelineLayout(desc);
        pipelines_ = std::make_unique<Pipelines>(context.Device(), *layout_, pipelineList);
    }

    Draws::~Draws() = default;

    plume::RenderBufferReference Draws::Shared(const SharedConstants& shared)
    {
        // SharedConstants has no padding (its size is asserted), so equal values have equal bytes.
        if (shared_.submission != context_.OpenSubmission() || memcmp(&shared_.values, &shared, sizeof(shared)) != 0)
        {
            constexpr uint64_t kBytes = (sizeof(SharedConstants) + Context::kConstantAlignment - 1) & ~(Context::kConstantAlignment - 1);
            shared_.upload = context_.Allocate(kBytes, Context::kConstantAlignment);
            memcpy(shared_.upload.data, &shared, sizeof(shared));
            shared_.values = shared;
            shared_.submission = context_.OpenSubmission();
        }
        return shared_.upload.Reference();
    }

    plume::RenderBufferReference Draws::Constants(ConstantUpload& last, const std::shared_ptr<const ConstantBlock>& block)
    {
        if (block != last.block || last.submission != context_.OpenSubmission())
        {
            last.upload = context_.Allocate(block->size(), Context::kConstantAlignment);
            memcpy(last.upload.data, block->data(), block->size());
            last.block = block;
            last.submission = context_.OpenSubmission();
        }
        return last.upload.Reference();
    }

    void Draws::Draw(DrawOp& op)
    {
        PipelineKey key;
        key.vertexShader = op.vertexShader;
        key.pixelShader = op.pixelShader;
        key.input = op.input->id;
        key.state = op.state;
        key.samples = op.color.key != 0 ? op.color.samples : op.depth.samples;
        key.targets = (op.color.key != 0 ? 1 : 0) | (op.depth.key != 0 ? 2 : 0);
        // Without a depth-stencil target the depth and stencil state does not reach the pipeline.
        if (op.depth.key == 0)
        {
            key.state.depthControl = 0;
            key.state.stencilMasks = 0;
            key.state.depthBias = 0;
            key.state.depthBiasSlope = 0;
        }
        const plume::RenderPipeline* pipeline = pipelines_->Get(key, op.input);
        if (pipeline == nullptr)
            return;
        const SurfaceDesc& target = op.color.key != 0 ? op.color : op.depth;
        Rect scissor = Intersect(op.scissor, { 0, 0, int32_t(target.width), int32_t(target.height) });
        if (scissor.Empty())
            return;
        plume::RenderCommandList& list = context_.Commands();

        // The state the last draw set stays unless the command list changed or something else drew in
        // between: every other drawing sets a pipeline of its own, which becomes Plume's active one.
        const auto& d3dList = static_cast<const plume::D3D12CommandList&>(list);
        if (state_.submission != context_.OpenSubmission() ||
            d3dList.activeGraphicsPipelineLayout != static_cast<const plume::D3D12PipelineLayout*>(layout_.get()) ||
            static_cast<const plume::RenderPipeline*>(d3dList.activeGraphicsPipeline) != state_.pipeline)
        {
            state_ = ListState();
            state_.submission = context_.OpenSubmission();
        }

        // Vertex streams and indices: the copied ones from the upload buffers, the hosted ones from their
        // host copies; a draw whose host copy is missing is skipped.
        Context::Upload vertices;
        if (!op.vertices.empty())
        {
            vertices = context_.Allocate(op.vertices.size(), 16);
            memcpy(vertices.data, op.vertices.data(), op.vertices.size());
        }
        const VertexInput& input = *op.input;
        VertexStreams streams;
        streams.count = uint32_t(input.strides.size());
        for (uint32_t i = 0; i < streams.count; i++)
        {
            const HostedStream& hosted = op.hosted[i];
            if (hosted.bytes == 0)
            {
                uint32_t end = i + 1 < streams.count ? op.streamOffsets[i + 1] : uint32_t(op.vertices.size());
                streams.views[i] = plume::RenderVertexBufferView(
                    plume::RenderBufferReference(vertices.buffer, vertices.offset + op.streamOffsets[i]), end - op.streamOffsets[i]);
            }
            else if (plume::RenderBuffer* buffer = buffers_.Bind(list, BufferKey{ hosted.address, hosted.bytes }))
            {
                streams.views[i] = plume::RenderVertexBufferView(plume::RenderBufferReference(buffer, hosted.offset), hosted.size);
            }
            else
            {
                LOG_ONCE("gpu: a draw reads a vertex buffer the host has no copy of; it is skipped");
                return;
            }
            streams.slots[i] = plume::RenderInputSlot(i, input.strides[i]);
        }
        const HostedIndices& hostedIndices = op.hostedIndices;
        plume::RenderIndexBufferView indexView;
        if (hostedIndices.key.bytes != 0)
        {
            plume::RenderBuffer* buffer = buffers_.Bind(list, hostedIndices.key);
            if (buffer == nullptr)
            {
                LOG_ONCE("gpu: a draw reads an index buffer the host has no copy of; it is skipped");
                return;
            }
            bool index32 = hostedIndices.key.format == BufferFormat::Index32;
            indexView = plume::RenderIndexBufferView(plume::RenderBufferReference(buffer, 0),
                hostedIndices.key.bytes & (index32 ? ~3u : ~1u), index32 ? plume::RenderFormat::R32_UINT : plume::RenderFormat::R16_UINT);
        }
        else
        {
            Context::Upload indices = context_.Allocate(op.indices.size() * 4, 4);
            memcpy(indices.data, op.indices.data(), op.indices.size() * 4);
            indexView = plume::RenderIndexBufferView(indices.Reference(), uint32_t(op.indices.size() * 4),
                plume::RenderFormat::R32_UINT);
        }

        // A slot the last draw bound to the same texture keeps its descriptors and its barrier, unless
        // a texture was uploaded, released, resolved or read back since. A fresh state holds the
        // binding of no texture in every slot, which is what binding one gives.
        if (uint64_t changes = textures_.Changes(); state_.textureChanges != changes)
        {
            state_.textures = {};
            state_.bindings = {};
            state_.textureChanges = changes;
        }
        SharedConstants shared = op.shared;
        for (uint32_t slot = 0; slot < SharedConstants::kFetchSlots; slot++)
        {
            if (state_.textures[slot] != op.textures[slot])
            {
                state_.bindings[slot] = textures_.Bind(list, op.textures[slot]);
                state_.textures[slot] = op.textures[slot];
            }
            const TextureBinding& binding = state_.bindings[slot];
            shared.texture2D[slot] = binding.texture2D;
            shared.textureCube[slot] = binding.textureCube;
            shared.sampler[slot] = binding.sampler;
        }
        targets_.BindForDraw(list, op.color, op.depth);

        plume::RenderBufferReference vertexConstants = Constants(vertexConstants_, op.vertexConstants);
        plume::RenderBufferReference pixelConstants = Constants(pixelConstants_, op.pixelConstants);
        plume::RenderBufferReference sharedConstants = Shared(shared);

        if (!state_.layoutSet)
        {
            list.setGraphicsPipelineLayout(layout_.get());
            descriptors_.Bind(list);
            state_.layoutSet = true;
        }
        if (pipeline != state_.pipeline)
        {
            list.setPipeline(pipeline);
            state_.pipeline = pipeline;
        }
        SetRoot(list, state_.vertexConstants, vertexConstants, kVertexConstantsRoot);
        SetRoot(list, state_.pixelConstants, pixelConstants, kPixelConstantsRoot);
        SetRoot(list, state_.sharedConstants, sharedConstants, kSharedConstantsRoot);

        ID3D12GraphicsCommandList* d3d = d3dList.d3d;
        bool alphaOnly = false;
        if (op.color.key != 0 && UsesBlendColor(op.state.blendControl, alphaOnly))
        {
            std::array<float, 4> color = op.blendColor;
            if (alphaOnly)
                std::fill(color.begin(), color.begin() + 3, op.blendColor[3]);
            if (!state_.blendFactor || *state_.blendFactor != color)
            {
                d3d->OMSetBlendFactor(color.data());
                state_.blendFactor = color;
            }
        }
        // Plume sets the reference of the pipeline when it differs from the last one it set; every
        // pipeline has 0, so it never overrides this one.
        if (op.depth.key != 0 && (op.state.depthControl & kStencilEnable) != 0 && state_.stencilReference != op.stencilReference)
        {
            d3d->OMSetStencilRef(op.stencilReference);
            state_.stencilReference = op.stencilReference;
        }

        const plume::RenderViewport viewport(op.viewport[0], op.viewport[1], op.viewport[2], op.viewport[3], op.viewport[4],
            op.viewport[5]);
        if (!state_.viewport || *state_.viewport != viewport)
        {
            list.setViewports(&viewport, 1);
            state_.viewport = viewport;
        }
        if (!state_.scissor || *state_.scissor != scissor)
        {
            const plume::RenderRect rect(scissor.left, scissor.top, scissor.right, scissor.bottom);
            list.setScissors(&rect, 1);
            state_.scissor = scissor;
        }
        if (!state_.streams || !Same(*state_.streams, streams))
        {
            list.setVertexBuffers(0, streams.views.data(), streams.count, streams.slots.data());
            state_.streams = streams;
        }
        if (!state_.indices || !Same(*state_.indices, indexView))
        {
            list.setIndexBuffer(&indexView);
            state_.indices = indexView;
        }
        if (hostedIndices.key.bytes != 0)
            list.drawIndexedInstanced(hostedIndices.count, 1, hostedIndices.first, hostedIndices.baseVertex, 0);
        else
            list.drawIndexedInstanced(uint32_t(op.indices.size()), 1, 0, 0, 0);
    }
}
