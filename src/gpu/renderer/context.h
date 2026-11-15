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
    // The D3D12 device and queue, and the recording of GPU work. Owner thread only, except for the
    // completion thread it runs.
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
        // Submits the open command list, if there is one.
        void Flush();
        // Flushes; completion(token) runs once the GPU has finished all the work so far.
        void Report(uint32_t token);
        // Flushes and waits until the queue is idle, including work queued after the last
        // submission (Presents).
        void WaitIdle();
        // Keeps an object alive until the GPU has finished the work recorded so far.
        void Retire(std::shared_ptr<void> object);

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

        List& AcquireList();
        uint64_t Submit(plume::RenderCommandList& list);
        void WaitSubmission(uint64_t submission);
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
