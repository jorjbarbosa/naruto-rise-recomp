#pragma once

#include "gpu/renderer/renderer.h"

#include <plume_render_interface.h>

#include <cstdint>
#include <map>
#include <memory>
#include <utility>

namespace gpu
{
    class Context;

    // The host copies of guest vertex buffers, by (GPU address, size), filled from the uploads the
    // guest side posts. Owner thread only.
    class Buffers
    {
    public:
        explicit Buffers(Context& context) : context_(context) {}

        // Replaces the copy of (address, data size) with a new buffer holding the data; the old one
        // is retired, so draws recorded before keep reading it.
        void Upload(const BufferUpload& upload);
        // Releases the copies that overlap the GPU addresses [begin, end), once the GPU no longer
        // uses them.
        void Release(uint32_t begin, uint32_t end);
        // Releases the copy of (address, bytes), once the GPU no longer uses it.
        void Forget(uint32_t address, uint32_t bytes);
        // The copy of (address, bytes), made readable by the vertex input; null when there is none.
        plume::RenderBuffer* Bind(plume::RenderCommandList& list, uint32_t address, uint32_t bytes);

    private:
        using Key = std::pair<uint32_t, uint32_t>;

        Context& context_;
        std::map<Key, std::shared_ptr<plume::RenderBuffer>> buffers_;
    };
}
