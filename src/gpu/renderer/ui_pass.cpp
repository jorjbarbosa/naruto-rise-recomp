// The host UI's textures and triangles, drawn over the presented image.

#include "gpu/renderer/ui_pass.h"

#include "gpu/renderer/context.h"
#include "gpu/renderer/level_copy.h"
#include "gpu/renderer/passes.h"
#include "log.h"

#include <plume_d3d12.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <optional>

#include "shaders/ui_ps.h"
#include "shaders/ui_vs.h"

namespace gpu
{
    namespace
    {
        // Root constants of ui_vs.hlsl and ui_ps.hlsl.
        struct Constants
        {
            float transform[16]; // column-major: projection x the draw's transform
            float translation[2];
            uint32_t straightAlpha;
            uint32_t pad;
        };

        const plume::RenderDescriptorRange kSamplerRange(plume::RenderDescriptorRangeType::SAMPLER, 0, 1);
        const plume::RenderDescriptorSetDesc kSamplerSet(&kSamplerRange, 1);

        // A format's storage: texels per block side and bytes per block (a texel for RGBA8).
        struct Storage
        {
            plume::RenderFormat format;
            uint32_t block;
            uint32_t bytesPerBlock;
            bool straightAlpha;
        };

        // None for a value that is no format.
        std::optional<Storage> StorageOf(ui::Format format)
        {
            switch (format)
            {
            case ui::Format::Rgba8Premultiplied:
                return Storage{ plume::RenderFormat::R8G8B8A8_UNORM, 1, 4, false };
            case ui::Format::Bc1:
                return Storage{ plume::RenderFormat::BC1_UNORM, 4, 8, true };
            case ui::Format::Bc2:
                return Storage{ plume::RenderFormat::BC2_UNORM, 4, 16, true };
            case ui::Format::Bc3:
                return Storage{ plume::RenderFormat::BC3_UNORM, 4, 16, true };
            }
            return std::nullopt;
        }

        bool IndicesInFrame(const ui::Frame& frame, const ui::Draw& draw)
        {
            return uint64_t(draw.firstIndex) + draw.indexCount <= frame.indices.size();
        }

        // Whether the draw's texture coordinates leave [0, 1]: a texture repeated by coordinate, as
        // RmlUi's tiled decorators do. The draw's indices lie in the frame.
        bool Repeats(const ui::Frame& frame, const ui::Draw& draw)
        {
            for (uint32_t i = draw.firstIndex; i < draw.firstIndex + draw.indexCount; i++)
            {
                int64_t vertex = int64_t(draw.baseVertex) + frame.indices[i];
                if (vertex < 0 || vertex >= int64_t(frame.vertices.size()))
                    continue;
                const ui::Vertex& v = frame.vertices[size_t(vertex)];
                if (v.u < 0.0f || v.u > 1.0f || v.v < 0.0f || v.v > 1.0f)
                    return true;
            }
            return false;
        }

        // a x b, column-major 4x4 matrices.
        void Multiply(const float (&a)[16], const float (&b)[16], float (&out)[16])
        {
            for (int column = 0; column < 4; column++)
                for (int row = 0; row < 4; row++)
                {
                    float sum = 0.0f;
                    for (int k = 0; k < 4; k++)
                        sum += a[k * 4 + row] * b[column * 4 + k];
                    out[column * 4 + row] = sum;
                }
        }
    }

    UiPass::UiPass(Context& context, const Passes& passes, plume::RenderFormat format)
        : context_(context), passes_(passes)
    {
        plume::RenderDevice& device = context.Device();

        // Linear, clamped at the edges, where wrapping would bleed the opposite edge into an image's
        // border; wrapped for the draws that repeat a texture.
        auto sampler = [&](plume::RenderTextureAddressMode mode, Sampler& out) {
            plume::RenderSamplerDesc desc;
            desc.minFilter = plume::RenderFilter::LINEAR;
            desc.magFilter = plume::RenderFilter::LINEAR;
            desc.mipmapMode = plume::RenderMipmapMode::LINEAR;
            desc.addressU = mode;
            desc.addressV = mode;
            desc.addressW = mode;
            desc.shaderVisibility = plume::RenderShaderVisibility::PIXEL;
            out.sampler = device.createSampler(desc);
            out.set = device.createDescriptorSet(kSamplerSet);
            out.set->setSampler(0, out.sampler.get());
        };
        sampler(plume::RenderTextureAddressMode::CLAMP, clamp_);
        sampler(plume::RenderTextureAddressMode::WRAP, wrap_);

        // The constants, the texture in set 0, the sampler in set 1, vertices from the input
        // assembler.
        const plume::RenderDescriptorSetDesc sets[] = { Passes::TextureSetDesc(), kSamplerSet };
        const plume::RenderPushConstantRange constants(0, 0, 0, sizeof(Constants),
            plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::PIXEL);
        layout_ = device.createPipelineLayout(plume::RenderPipelineLayoutDesc(&constants, 1, sets, 2, false, true));

        auto vertex = device.createShader(kUiVs, sizeof(kUiVs), "main", plume::RenderShaderFormat::DXIL);
        auto pixel = device.createShader(kUiPs, sizeof(kUiPs), "main", plume::RenderShaderFormat::DXIL);
        const plume::RenderInputSlot slot(0, sizeof(ui::Vertex));
        const plume::RenderInputElement elements[] = {
            { "POSITION", 0, 0, plume::RenderFormat::R32G32_FLOAT, 0, offsetof(ui::Vertex, x) },
            { "COLOR", 0, 1, plume::RenderFormat::R8G8B8A8_UNORM, 0, offsetof(ui::Vertex, colour) },
            { "TEXCOORD", 0, 2, plume::RenderFormat::R32G32_FLOAT, 0, offsetof(ui::Vertex, u) },
        };
        // Premultiplied alpha, as RmlUi blends.
        plume::RenderBlendDesc blend;
        blend.blendEnabled = true;
        blend.srcBlend = plume::RenderBlend::ONE;
        blend.dstBlend = plume::RenderBlend::INV_SRC_ALPHA;
        blend.blendOp = plume::RenderBlendOperation::ADD;
        blend.srcBlendAlpha = plume::RenderBlend::ONE;
        blend.dstBlendAlpha = plume::RenderBlend::INV_SRC_ALPHA;
        blend.blendOpAlpha = plume::RenderBlendOperation::ADD;

        plume::RenderGraphicsPipelineDesc desc;
        desc.pipelineLayout = layout_.get();
        desc.vertexShader = vertex.get();
        desc.pixelShader = pixel.get();
        desc.renderTargetFormat[0] = format;
        desc.renderTargetBlend[0] = blend;
        desc.renderTargetCount = 1;
        desc.inputSlots = &slot;
        desc.inputSlotsCount = 1;
        desc.inputElements = elements;
        desc.inputElementsCount = uint32_t(std::size(elements));
        pipeline_ = device.createGraphicsPipeline(desc);
        if (static_cast<plume::D3D12GraphicsPipeline*>(pipeline_.get())->d3d == nullptr)
            Fatal("gpu: cannot create the UI pipeline");

        CreateWhite();
    }

    UiPass::~UiPass() = default;

    void UiPass::CreateWhite()
    {
        ui::TextureDesc desc;
        desc.width = 1;
        desc.height = 1;
        desc.data = { 255, 255, 255, 255 };
        white_ = Create(desc);
        if (white_ == nullptr)
            Fatal("gpu: cannot create the UI's white texture");
    }

    std::unique_ptr<UiPass::Texture> UiPass::Create(const ui::TextureDesc& desc)
    {
        const std::optional<Storage> format = StorageOf(desc.format);
        uint32_t largest = std::max(desc.width, desc.height);
        uint32_t maxLevels = largest == 0 ? 0 : std::bit_width(largest);
        if (!format || desc.width == 0 || desc.height == 0 || desc.levels == 0 || desc.levels > maxLevels ||
            desc.width % format->block != 0 || desc.height % format->block != 0)
        {
            Log("gpu: a UI texture of %ux%u, %u levels, format %u is not valid", desc.width, desc.height, desc.levels,
                unsigned(desc.format));
            return nullptr;
        }
        const Storage& storage = *format;
        size_t expected = 0;
        for (uint32_t level = 0; level < desc.levels; level++)
        {
            uint32_t columns = (std::max(desc.width >> level, 1u) + storage.block - 1) / storage.block;
            uint32_t rows = (std::max(desc.height >> level, 1u) + storage.block - 1) / storage.block;
            expected += size_t(columns) * storage.bytesPerBlock * rows;
        }
        if (desc.data.size() != expected)
        {
            Log("gpu: a UI texture of %ux%u has %zu bytes, not %zu", desc.width, desc.height, desc.data.size(), expected);
            return nullptr;
        }

        auto texture = std::make_unique<Texture>();
        texture->straightAlpha = storage.straightAlpha;
        texture->texture = context_.Device().createTexture(
            plume::RenderTextureDesc::Texture2D(desc.width, desc.height, desc.levels, storage.format));
        if (static_cast<plume::D3D12Texture*>(texture->texture.get())->d3d == nullptr)
        {
            Log("gpu: cannot create a UI texture of %ux%u", desc.width, desc.height);
            return nullptr;
        }
        texture->sampling = passes_.CreateTextureSet(*texture->texture, nullptr);

        plume::RenderCommandList& list = context_.Commands();
        list.barriers(plume::RenderBarrierStage::COPY,
            plume::RenderTextureBarrier(texture->texture.get(), plume::RenderTextureLayout::COPY_DEST));
        const uint8_t* source = desc.data.data();
        for (uint32_t level = 0; level < desc.levels; level++)
        {
            uint32_t width = std::max(desc.width >> level, 1u);
            uint32_t height = std::max(desc.height >> level, 1u);
            uint32_t rows = (height + storage.block - 1) / storage.block;
            uint32_t rowBytes = (width + storage.block - 1) / storage.block * storage.bytesPerBlock;
            CopyLevel(context_, list, *texture->texture, level, 0, storage.format, width, height,
                { source, rowBytes, rows, storage.block, storage.bytesPerBlock });
            source += size_t(rowBytes) * rows;
        }
        list.barriers(plume::RenderBarrierStage::GRAPHICS,
            plume::RenderTextureBarrier(texture->texture.get(), plume::RenderTextureLayout::SHADER_READ));
        return texture;
    }

    void UiPass::BeginPresent()
    {
        ui::Updates updates = ui::TakeUpdates();
        for (auto& [handle, desc] : updates.creates)
            if (std::unique_ptr<Texture> texture = Create(desc))
                textures_[handle] = std::move(texture);
        if (updates.frame)
        {
            frame_ = std::move(updates.frame);
            // The sampler of each draw, once per frame rather than at each present.
            repeats_.assign(frame_->draws.size(), false);
            for (size_t i = 0; i < frame_->draws.size(); i++)
                repeats_[i] = IndicesInFrame(*frame_, frame_->draws[i]) && Repeats(*frame_, frame_->draws[i]);
        }
        releases_.insert(releases_.end(), updates.releases.begin(), updates.releases.end());
    }

    void UiPass::EndPresent()
    {
        // A texture the frame still names stays until a frame that does not replaces it: the frame
        // is drawn again at every present until then.
        std::erase_if(releases_, [&](ui::Texture handle) {
            if (frame_ && std::any_of(frame_->draws.begin(), frame_->draws.end(),
                              [&](const ui::Draw& draw) { return draw.texture == handle; }))
                return false;
            if (auto it = textures_.find(handle); it != textures_.end())
            {
                context_.Retire(std::shared_ptr<Texture>(std::move(it->second)));
                textures_.erase(it);
            }
            return true;
        });
        if (frame_ && (!frame_->draws.empty() || frame_->blur > 0.0f))
            ui::NotifyPresented();
    }

    float UiPass::Blur() const
    {
        return frame_ ? std::clamp(frame_->blur, 0.0f, 1.0f) : 0.0f;
    }

    void UiPass::Draw(plume::RenderCommandList& list, uint32_t width, uint32_t height)
    {
        if (!frame_ || frame_->draws.empty() || frame_->vertices.empty() || frame_->indices.empty() ||
            frame_->width == 0 || frame_->height == 0)
            return;
        const ui::Frame& frame = *frame_;

        uint32_t vertexBytes = uint32_t(frame.vertices.size() * sizeof(ui::Vertex));
        uint32_t indexBytes = uint32_t(frame.indices.size() * sizeof(uint32_t));
        Context::Upload vertices = context_.Allocate(vertexBytes, 16);
        Context::Upload indices = context_.Allocate(indexBytes, 16);
        memcpy(vertices.data, frame.vertices.data(), vertexBytes);
        memcpy(indices.data, frame.indices.data(), indexBytes);
        const plume::RenderVertexBufferView vertexView(vertices.Reference(), vertexBytes);
        const plume::RenderIndexBufferView indexView(indices.Reference(), indexBytes, plume::RenderFormat::R32_UINT);
        const plume::RenderInputSlot slot(0, sizeof(ui::Vertex));

        list.setGraphicsPipelineLayout(layout_.get());
        list.setPipeline(pipeline_.get());
        list.setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
        list.setVertexBuffers(0, &vertexView, 1, &slot);
        list.setIndexBuffer(&indexView);

        // Frame pixels to clip space; the viewport stretches the frame over the target when their
        // sizes differ. Depth is flattened, so a transform with depth is never clipped by it.
        float projection[16] = {};
        projection[0] = 2.0f / float(frame.width);
        projection[5] = -2.0f / float(frame.height);
        projection[12] = -1.0f;
        projection[13] = 1.0f;
        projection[15] = 1.0f;
        const float scaleX = float(width) / float(frame.width);
        const float scaleY = float(height) / float(frame.height);
        const plume::RenderRect whole(0, 0, int32_t(width), int32_t(height));

        for (size_t i = 0; i < frame.draws.size(); i++)
        {
            const ui::Draw& draw = frame.draws[i];
            if (draw.indexCount == 0)
                continue;
            if (!IndicesInFrame(frame, draw))
            {
                LOG_ONCE("gpu: a UI draw reads past the frame's indices; it is skipped");
                continue;
            }
            const Texture* texture = white_.get();
            if (draw.texture != ui::kNoTexture)
            {
                auto it = textures_.find(draw.texture);
                if (it == textures_.end())
                {
                    LOG_ONCE("gpu: a UI draw names a texture that does not exist; it is skipped");
                    continue;
                }
                texture = it->second.get();
            }

            plume::RenderRect scissor = whole;
            if (draw.scissored)
            {
                scissor.left = std::clamp(int32_t(std::floor(draw.scissor.left * scaleX)), 0, int32_t(width));
                scissor.top = std::clamp(int32_t(std::floor(draw.scissor.top * scaleY)), 0, int32_t(height));
                scissor.right = std::clamp(int32_t(std::ceil(draw.scissor.right * scaleX)), 0, int32_t(width));
                scissor.bottom = std::clamp(int32_t(std::ceil(draw.scissor.bottom * scaleY)), 0, int32_t(height));
                if (scissor.left >= scissor.right || scissor.top >= scissor.bottom)
                    continue;
            }

            Constants constants{};
            Multiply(projection, draw.transform, constants.transform);
            constants.translation[0] = draw.translation[0];
            constants.translation[1] = draw.translation[1];
            constants.straightAlpha = texture->straightAlpha ? 1 : 0;
            list.setScissors(scissor);
            list.setGraphicsDescriptorSet(texture->sampling.get(), 0);
            list.setGraphicsDescriptorSet((repeats_[i] ? wrap_ : clamp_).set.get(), 1);
            list.setGraphicsPushConstants(0, &constants, 0, sizeof(constants));
            list.drawIndexedInstanced(draw.indexCount, 1, draw.firstIndex, draw.baseVertex, 0);
        }
    }
}
