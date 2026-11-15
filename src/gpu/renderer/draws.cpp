// Guest draws: recording of each draw over its pipeline, with its constants, vertices and indices.

#include "gpu/renderer/draws.h"

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
    }

    Draws::Draws(Context& context, Targets& targets, Descriptors& descriptors, Textures& textures,
        const std::filesystem::path& pipelineList)
        : context_(context), targets_(targets), descriptors_(descriptors), textures_(textures)
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

        SharedConstants shared = op.shared;
        for (uint32_t slot = 0; slot < SharedConstants::kFetchSlots; slot++)
            textures_.Bind(list, slot, op.textures[slot], shared);
        targets_.BindForDraw(list, op.color, op.depth);

        // Per-draw data in the upload buffers.
        constexpr uint64_t kSharedBytes = (sizeof(SharedConstants) + Context::kConstantAlignment - 1) & ~(Context::kConstantAlignment - 1);
        Context::Upload constants = context_.Allocate(2 * op.vertexConstants.size() + kSharedBytes, Context::kConstantAlignment);
        memcpy(constants.data, op.vertexConstants.data(), op.vertexConstants.size());
        memcpy(constants.data + op.vertexConstants.size(), op.pixelConstants.data(), op.pixelConstants.size());
        memcpy(constants.data + 2 * op.vertexConstants.size(), &shared, sizeof(shared));
        Context::Upload vertices = context_.Allocate(std::max<size_t>(op.vertices.size(), 4), 16);
        memcpy(vertices.data, op.vertices.data(), op.vertices.size());
        Context::Upload indices = context_.Allocate(op.indices.size() * 4, 4);
        memcpy(indices.data, op.indices.data(), op.indices.size() * 4);

        list.setGraphicsPipelineLayout(layout_.get());
        list.setPipeline(pipeline);
        descriptors_.Bind(list);
        list.setGraphicsRootDescriptor(constants.Reference(), kVertexConstantsRoot);
        list.setGraphicsRootDescriptor({ constants.buffer, constants.offset + op.vertexConstants.size() }, kPixelConstantsRoot);
        list.setGraphicsRootDescriptor({ constants.buffer, constants.offset + 2 * op.vertexConstants.size() },
            kSharedConstantsRoot);

        ID3D12GraphicsCommandList* d3d = static_cast<plume::D3D12CommandList&>(list).d3d;
        bool alphaOnly = false;
        if (op.color.key != 0 && UsesBlendColor(op.state.blendControl, alphaOnly))
        {
            float color[4] = { op.blendColor[0], op.blendColor[1], op.blendColor[2], op.blendColor[3] };
            if (alphaOnly)
                std::fill(color, color + 3, op.blendColor[3]);
            d3d->OMSetBlendFactor(color);
        }
        // Plume sets the reference of the pipeline when it differs from the last one it set; every
        // pipeline has 0, so it never overrides this one.
        if (op.depth.key != 0 && (op.state.depthControl & kStencilEnable) != 0)
            d3d->OMSetStencilRef(op.stencilReference);

        const plume::RenderViewport viewport(op.viewport[0], op.viewport[1], op.viewport[2], op.viewport[3], op.viewport[4],
            op.viewport[5]);
        const plume::RenderRect rect(scissor.left, scissor.top, scissor.right, scissor.bottom);
        list.setViewports(&viewport, 1);
        list.setScissors(&rect, 1);

        const VertexInput& input = *op.input;
        std::vector<plume::RenderVertexBufferView> views;
        std::vector<plume::RenderInputSlot> slots;
        for (uint32_t i = 0; i < input.strides.size(); i++)
        {
            uint32_t end = i + 1 < op.streamOffsets.size() ? op.streamOffsets[i + 1] : uint32_t(op.vertices.size());
            views.emplace_back(plume::RenderBufferReference(vertices.buffer, vertices.offset + op.streamOffsets[i]),
                end - op.streamOffsets[i]);
            slots.emplace_back(i, input.strides[i]);
        }
        list.setVertexBuffers(0, views.data(), uint32_t(views.size()), slots.data());
        const plume::RenderIndexBufferView indexView(indices.Reference(), uint32_t(op.indices.size() * 4),
            plume::RenderFormat::R32_UINT);
        list.setIndexBuffer(&indexView);
        list.drawIndexedInstanced(uint32_t(op.indices.size()), 1, 0, 0, 0);
    }
}
