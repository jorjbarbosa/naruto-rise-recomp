#pragma once

#include "gpu/renderer/renderer.h"

#include <plume_render_interface.h>

#include <cstdint>
#include <map>
#include <memory>

namespace gpu
{
    class Context;

    // The host copies of guest vertex and index buffers, by BufferKey, filled from the uploads the
    // guest side posts. Owner thread only.
    class Buffers
    {
    public:
        explicit Buffers(Context& context) : context_(context) {}

        // Replaces the copy of upload.key with a new buffer holding the data; the old one is retired,
        // so draws recorded before keep reading it.
        void Upload(const BufferUpload& upload);
        // Releases the copies that overlap the GPU addresses [begin, end), once the GPU no longer
        // uses them.
        void Release(uint32_t begin, uint32_t end);
        // Releases the copy of a buffer, once the GPU no longer uses it.
        void Forget(const BufferKey& key);
        // The copy of a buffer, made readable by the vertex input (vertex buffers) or the index input
        // (index buffers); null when there is none.
        plume::RenderBuffer* Bind(plume::RenderCommandList& list, const BufferKey& key);

    private:
        Context& context_;
        std::map<BufferKey, std::shared_ptr<plume::RenderBuffer>> buffers_; // ordered by address first
    };
}
