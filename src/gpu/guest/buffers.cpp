// Vertex buffers on the host: which buffers a draw reads from the host's copy, and their uploads.

#include "gpu/guest/buffers.h"

#include "gpu/gpu.h"
#include "gpu/guest/resources.h"
#include "gpu/guest/watch.h"
#include "gpu/renderer/renderer.h"
#include "memory.h"

#include <algorithm>
#include <map>
#include <mutex>

namespace gpu::guest
{
    namespace
    {
        // A vertex buffer (GPU address, bytes). Its memory is checked for writes at its first use in
        // a frame and after the last unlock of a vertex buffer over it, as the textures are.
        struct Buffer
        {
            uint64_t lastCheck = 0;      // the previous write check; 0: none
            uint64_t lastCheckFrame = 0; // the frame it was made in
            bool recheck = false;        // a vertex buffer over it was unlocked since
            bool hosted = false;         // the host's copy holds the memory as of that check
        };

        // Posts to the renderer happen under g_mutex, so that the uploads and releases of one buffer
        // reach the owner thread in the order the entries changed.
        std::mutex g_mutex; // the state below (the render thread draws, any thread unlocks or frees)
        std::map<std::pair<uint32_t, uint32_t>, Buffer> g_buffers; // ordered by address
        uint32_t g_largest = 0; // the largest size of an entry ever made, which bounds the overlap scans
        uint64_t g_frame = 1;
        // The latest write check when the previous frame began.
        uint64_t g_previousFrameStart = 0;
        uint64_t g_frameStart = 0;
        // Entries not hosted and unused for this many frames are dropped (the sub-ranges of the dynamic
        // vertex rings are new keys every frame).
        constexpr uint64_t kIdleFrames = 2;

        // Calls f(iterator) for each entry that overlaps the GPU addresses [begin, end); f returns the
        // next iterator. Under g_mutex.
        template <typename F>
        void ForEachOverlap(uint32_t begin, uint32_t end, F&& f)
        {
            uint32_t first = begin > g_largest ? begin - g_largest : 0;
            for (auto it = g_buffers.lower_bound({ first, 0 }); it != g_buffers.end() && it->first.first < end;)
            {
                if (uint64_t(it->first.first) + it->first.second > begin)
                    it = f(it);
                else
                    ++it;
            }
        }
    }

    bool UseHostedBuffer(uint32_t address, uint32_t bytes)
    {
        Renderer* renderer = ActiveRenderer();
        if (renderer == nullptr || bytes == 0 || uint64_t(address) + bytes > kPhysicalEnd)
            return false;
        std::lock_guard lock(g_mutex);
        auto it = g_buffers.find({ address, bytes });
        if (it == g_buffers.end())
        {
            // Memory a check found written in this frame or the previous one is dynamic: copied by the
            // draws, without an entry or a check of its own.
            if (LastWriteFound(address, bytes) > g_previousFrameStart)
                return false;
            it = g_buffers.emplace(std::pair(address, bytes), Buffer()).first;
            g_largest = std::max(g_largest, bytes);
        }
        Buffer& buffer = it->second;
        // Checked in this frame: hosted, or written in this frame and copied by every draw of it
        // (dynamic vertices).
        if (buffer.lastCheckFrame == g_frame && !buffer.recheck)
            return buffer.hosted;
        bool sinceEarlierFrame = buffer.lastCheck != 0 && buffer.lastCheckFrame < g_frame;
        uint64_t check = 0;
        bool changed = CheckWrites({ { address, bytes } }, buffer.lastCheck, check);
        buffer.lastCheck = check;
        buffer.lastCheckFrame = g_frame;
        buffer.recheck = false;
        if (changed)
        {
            // The stale copy goes; the buffer is hosted again once it is stable.
            if (buffer.hosted)
                renderer->ReleaseBuffer(address, bytes);
            buffer.hosted = false;
            return false;
        }
        // A buffer gets a host copy once it went from a check in an earlier frame to this one
        // without writes.
        if (buffer.hosted || !sinceEarlierFrame)
            return buffer.hosted;

        // The copy holds every write the check found; a later write is found by a later check.
        auto upload = std::make_unique<BufferUpload>();
        upload->address = address;
        const uint8_t* memory = mem::Ptr(GuestPhysical(address));
        upload->data.assign(memory, memory + bytes);
        renderer->UploadBuffer(std::move(upload));
        buffer.hosted = true;
        return true;
    }

    void ForgetBuffers(uint32_t begin, uint32_t end)
    {
        std::lock_guard lock(g_mutex);
        ForEachOverlap(begin, end, [](auto it) { return g_buffers.erase(it); });
        if (Renderer* renderer = ActiveRenderer())
            renderer->ReleaseBuffers(begin, end);
    }

    void InvalidateBuffers(uint32_t begin, uint32_t end)
    {
        std::lock_guard lock(g_mutex);
        ForEachOverlap(begin, end, [](auto it) {
            it->second.recheck = true;
            return ++it;
        });
    }

    void EndBufferFrame()
    {
        std::lock_guard lock(g_mutex);
        std::erase_if(g_buffers,
            [](const auto& entry) { return !entry.second.hosted && entry.second.lastCheckFrame + kIdleFrames < g_frame; });
        g_frame++;
        g_previousFrameStart = g_frameStart;
        g_frameStart = LatestCheck();
    }
}
