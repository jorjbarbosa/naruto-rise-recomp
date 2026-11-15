// Host copies of guest vertex buffers: default-heap buffers filled through the upload buffers.

#include "gpu/renderer/buffers.h"

#include "gpu/renderer/context.h"
#include "log.h"

#include <plume_d3d12.h>

#include <cstring>

namespace gpu
{
    void Buffers::Upload(const BufferUpload& upload)
    {
        Key key{ upload.address, uint32_t(upload.data.size()) };
        std::shared_ptr<plume::RenderBuffer> buffer = context_.Device().createBuffer(
            plume::RenderBufferDesc::VertexBuffer(upload.data.size(), plume::RenderHeapType::DEFAULT));
        if (static_cast<plume::D3D12Buffer*>(buffer.get())->d3d == nullptr)
        {
            LOG_ONCE("gpu: a vertex buffer of %zu bytes cannot be created", upload.data.size());
            Forget(key.first, key.second);
            return;
        }
        Context::Upload staging = context_.Allocate(upload.data.size(), 16);
        memcpy(staging.data, upload.data.data(), upload.data.size());
        plume::RenderCommandList& list = context_.Commands();
        list.barriers(plume::RenderBarrierStage::COPY, plume::RenderBufferBarrier(buffer.get(), plume::RenderBufferAccess::WRITE));
        list.copyBufferRegion(plume::RenderBufferReference(buffer.get(), 0), staging.Reference(), upload.data.size());

        auto it = buffers_.find(key);
        if (it != buffers_.end())
        {
            context_.Retire(std::move(it->second));
            it->second = std::move(buffer);
        }
        else
        {
            buffers_.emplace(key, std::move(buffer));
        }
    }

    void Buffers::Release(uint32_t begin, uint32_t end)
    {
        for (auto it = buffers_.begin(); it != buffers_.end() && it->first.first < end;)
        {
            if (uint64_t(it->first.first) + it->first.second > begin)
            {
                context_.Retire(std::move(it->second));
                it = buffers_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void Buffers::Forget(uint32_t address, uint32_t bytes)
    {
        auto it = buffers_.find({ address, bytes });
        if (it == buffers_.end())
            return;
        context_.Retire(std::move(it->second));
        buffers_.erase(it);
    }

    plume::RenderBuffer* Buffers::Bind(plume::RenderCommandList& list, uint32_t address, uint32_t bytes)
    {
        auto it = buffers_.find({ address, bytes });
        if (it == buffers_.end())
            return nullptr;
        list.barriers(plume::RenderBarrierStage::GRAPHICS,
            plume::RenderBufferBarrier(it->second.get(), plume::RenderBufferAccess::READ));
        return it->second.get();
    }
}
