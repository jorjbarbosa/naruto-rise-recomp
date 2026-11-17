#pragma once

#include <plume_render_interface.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace gpu
{
    // The device and queue of the API under the renderer (backend.h), and the recording of GPU work.
    // Owner thread only, except for the completion thread it runs.
    //
    // Work is recorded into one open command list and submitted by Flush. The completion thread
    // waits for each submission in order and reports the tokens queued after it, so a token is
    // reported once everything submitted before it has completed.
    class Context
    {
    public:
        using Completion = std::function<void(uint32_t token)>;

        Context(std::unique_ptr<plume::RenderDevice> device, Completion completion);
        // Waits for the queue to go idle.
        ~Context();

        plume::RenderDevice& Device() { return *device_; }
        plume::RenderCommandQueue& Queue() { return *queue_; }

        // The open command list, begun on first use.
        plume::RenderCommandList& Commands();
        // Submits the open command list, if there is one. The submission waits for `wait` before its
        // graphics stages write color attachments and signals `signal` once it completes; with a
        // semaphore, a list is submitted even when nothing was recorded.
        void Flush(plume::RenderCommandSemaphore* wait = nullptr, plume::RenderCommandSemaphore* signal = nullptr);
        // Flushes; completion(token) runs once the GPU has finished all the work so far.
        void Report(uint32_t token);
        // Flushes and waits until the queue is idle, including work queued after the last
        // submission (Presents), then releases the retired objects.
        void WaitIdle();
        // Keeps an object alive until the GPU has finished the work recorded so far.
        void Retire(std::shared_ptr<void> object);
        // Waits until the GPU has finished a submission (an OpenSubmission() number, once flushed).
        void WaitSubmission(uint64_t submission);

        // Space in a persistently mapped upload buffer for data the open command list reads,
        // valid until the GPU has finished the work recorded so far.
        struct Upload
        {
            plume::RenderBuffer* buffer = nullptr;
            uint64_t offset = 0;
            uint8_t* data = nullptr;

            plume::RenderBufferReference Reference() const { return { buffer, offset }; }
        };
        Upload Allocate(uint64_t size, uint64_t alignment);
        // The alignment of a constant buffer view's data.
        static constexpr uint64_t kConstantAlignment = 256;
        // Placed footprints of the copies between buffers and textures: each row starts at a multiple
        // of kTextureRowAlignment bytes, each subresource at a multiple of kTexturePlacementAlignment.
        static constexpr uint32_t kTextureRowAlignment = 256;
        static constexpr uint32_t kTexturePlacementAlignment = 512;
        // The number the open command list (or the next one) is submitted as. An Upload stays valid
        // for the work recorded while this number does not change.
        uint64_t OpenSubmission() const { return submitted_ + 1; }

        // The graphics state of the open command list (pipeline layout and pipeline, bindings,
        // dynamic state, viewport, scissor, vertex and index buffers) belongs to the recorder that
        // claimed it last. A recorder claims it before it sets any of it; true when the caller made
        // the last claim on this command list, so what it set since is still in place.
        bool ClaimGraphicsState(const void* recorder)
        {
            bool kept = graphicsRecorder_ == recorder && graphicsSubmission_ == OpenSubmission();
            graphicsRecorder_ = recorder;
            graphicsSubmission_ = OpenSubmission();
            return kept;
        }

    private:
        struct List
        {
            std::unique_ptr<plume::RenderCommandList> list;
            uint64_t submission = 0; // the submission that last used it
        };
        // A submission to wait for, or a token to report once the submissions before it completed.
        struct Pending
        {
            plume::RenderCommandFence* fence = nullptr;
            bool hasToken = false;
            uint32_t token = 0;
        };
        struct Retired
        {
            uint64_t submission;
            std::shared_ptr<void> object;
        };
        struct UploadChunk
        {
            std::unique_ptr<plume::RenderBuffer> buffer;
            uint8_t* data = nullptr;
            uint64_t size = 0;
            uint64_t submission = 0; // the submission that last read it
        };

        List& AcquireList();
        UploadChunk& AcquireChunk(uint64_t size);
        uint64_t Submit(plume::RenderCommandList& list, plume::RenderCommandSemaphore* wait = nullptr,
            plume::RenderCommandSemaphore* signal = nullptr);
        uint64_t Completed();
        void Push(const Pending& entry);
        void CompleterMain();

        std::unique_ptr<plume::RenderDevice> device_;
        std::unique_ptr<plume::RenderCommandQueue> queue_;
        Completion completion_;

        std::deque<List> lists_;
        List* open_ = nullptr;
        std::unique_ptr<plume::RenderCommandList> idleList_;
        uint64_t submitted_ = 0;
        std::deque<Retired> retired_;
        std::deque<UploadChunk> chunks_;
        UploadChunk* chunk_ = nullptr; // the chunk being filled
        uint64_t chunkCursor_ = 0;
        const void* graphicsRecorder_ = nullptr; // ClaimGraphicsState
        uint64_t graphicsSubmission_ = 0;

        std::thread completer_;
        std::mutex mutex_;
        std::condition_variable pendingCv_;
        std::condition_variable completedCv_;
        std::deque<Pending> pending_;
        std::vector<std::unique_ptr<plume::RenderCommandFence>> fences_;
        std::vector<plume::RenderCommandFence*> freeFences_;
        uint64_t completed_ = 0;
        bool stopping_ = false;
    };
}
