// Host copies of guest vertex and index buffers, filled through the upload buffers.

#include "gpu/renderer/buffers.h"

#include "gpu/renderer/backend.h"
#include "gpu/renderer/context.h"
#include "log.h"


#include <cstring>

namespace gpu
{
    void Buffers::Upload(const BufferUpload& upload)
    {
        uint64_t size = upload.data.size();
        // The flag decides the state a read puts the buffer in (vertex input or index input).
        plume::RenderBufferDesc desc = upload.key.format == BufferFormat::Vertex
            ? plume::RenderBufferDesc::VertexBuffer(size, plume::RenderHeapType::DEFAULT)
            : plume::RenderBufferDesc::IndexBuffer(size, plume::RenderHeapType::DEFAULT);
        std::shared_ptr<plume::RenderBuffer> buffer = context_.Device().createBuffer(desc);
        if (!IsCreated(buffer.get()))
        {
            LOG_ONCE("gpu: a host buffer of %llu bytes cannot be created", (unsigned long long)size);
            Forget(upload.key);
            return;
        }
        Context::Upload staging = context_.Allocate(size, 16);
        memcpy(staging.data, upload.data.data(), size);
        plume::RenderCommandList& list = context_.Commands();
        list.barriers(plume::RenderBarrierStage::COPY, plume::RenderBufferBarrier(buffer.get(), plume::RenderBufferAccess::WRITE));
        list.copyBufferRegion(plume::RenderBufferReference(buffer.get(), 0), staging.Reference(), size);

        auto it = buffers_.find(upload.key);
        if (it != buffers_.end())
        {
            context_.Retire(std::move(it->second));
            it->second = std::move(buffer);
        }
        else
        {
            buffers_.emplace(upload.key, std::move(buffer));
        }
    }

    void Buffers::Release(uint32_t begin, uint32_t end)
    {
        for (auto it = buffers_.begin(); it != buffers_.end() && it->first.address < end;)
        {
            if (uint64_t(it->first.address) + it->first.bytes > begin)
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

    void Buffers::Forget(const BufferKey& key)
    {
        auto it = buffers_.find(key);
        if (it == buffers_.end())
            return;
        context_.Retire(std::move(it->second));
        buffers_.erase(it);
    }

    plume::RenderBuffer* Buffers::Bind(plume::RenderCommandList& list, const BufferKey& key)
    {
        auto it = buffers_.find(key);
        if (it == buffers_.end())
            return nullptr;
        list.barriers(plume::RenderBarrierStage::GRAPHICS,
            plume::RenderBufferBarrier(it->second.get(), plume::RenderBufferAccess::READ));
        return it->second.get();
    }
}
