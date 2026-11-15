// Recording and submission of GPU work, and the completion thread.

#include "gpu/renderer/context.h"

#include "log.h"

#include <algorithm>

#include <windows.h>

namespace gpu
{
    namespace
    {
        // Command lists kept for recording. With one frame in flight a few are enough; when all
        // are in use the owner waits for the oldest.
        constexpr size_t kMaxLists = 4;
        // Upload buffers are made in chunks of this size, or of one allocation when larger.
        constexpr uint64_t kChunkSize = 8 << 20;
    }

    Context::Context(std::unique_ptr<plume::RenderDevice> device, Completion completion)
        : device_(std::move(device)), completion_(std::move(completion))
    {
        queue_ = device_->createCommandQueue(plume::RenderCommandListType::DIRECT);
        idleList_ = queue_->createCommandList();
        completer_ = std::thread([this] { CompleterMain(); });
        SetThreadDescription(static_cast<HANDLE>(completer_.native_handle()), L"gpu completion");
    }

    Context::~Context()
    {
        WaitIdle();
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        pendingCv_.notify_all();
        completer_.join();
        retired_.clear();
    }

    plume::RenderCommandList& Context::Commands()
    {
        if (open_ == nullptr)
        {
            open_ = &AcquireList();
            open_->list->begin();
        }
        return *open_->list;
    }

    Context::List& Context::AcquireList()
    {
        List* oldest = nullptr;
        for (List& list : lists_)
            if (oldest == nullptr || list.submission < oldest->submission)
                oldest = &list;
        if (oldest != nullptr && oldest->submission <= Completed())
            return *oldest;
        if (lists_.size() < kMaxLists)
            return lists_.emplace_back(List{ queue_->createCommandList(), 0 });
        WaitSubmission(oldest->submission);
        return *oldest;
    }

    void Context::Flush()
    {
        if (open_ != nullptr)
        {
            open_->list->end();
            open_->submission = Submit(*open_->list);
            open_ = nullptr;
        }
        uint64_t completed = Completed();
        while (!retired_.empty() && retired_.front().submission <= completed)
            retired_.pop_front();
    }

    void Context::Report(uint32_t token)
    {
        Flush();
        Push({ nullptr, true, token });
    }

    void Context::WaitIdle()
    {
        Flush();
        idleList_->begin();
        idleList_->end();
        WaitSubmission(Submit(*idleList_));
        retired_.clear();
    }

    void Context::Retire(std::shared_ptr<void> object)
    {
        // Work that used the object is either submitted or in the open list.
        retired_.push_back({ submitted_ + (open_ != nullptr ? 1 : 0), std::move(object) });
    }

    Context::Upload Context::Allocate(uint64_t size, uint64_t alignment)
    {
        Commands();
        uint64_t at = (chunkCursor_ + alignment - 1) & ~(alignment - 1);
        if (chunk_ == nullptr || at + size > chunk_->size)
        {
            chunk_ = &AcquireChunk(size);
            at = 0;
        }
        chunkCursor_ = at + size;
        chunk_->submission = submitted_ + 1; // the open list
        return { chunk_->buffer.get(), at, chunk_->data + at };
    }

    Context::UploadChunk& Context::AcquireChunk(uint64_t size)
    {
        uint64_t completed = Completed();
        for (UploadChunk& chunk : chunks_)
            if (&chunk != chunk_ && chunk.size >= size && chunk.submission <= completed)
                return chunk;
        UploadChunk chunk;
        chunk.size = std::max(size, kChunkSize);
        chunk.buffer = device_->createBuffer(plume::RenderBufferDesc::UploadBuffer(chunk.size,
            plume::RenderBufferFlag::VERTEX | plume::RenderBufferFlag::INDEX | plume::RenderBufferFlag::CONSTANT));
        chunk.data = static_cast<uint8_t*>(chunk.buffer->map());
        if (chunk.data == nullptr)
            Fatal("gpu: cannot map an upload buffer of %llu bytes", (unsigned long long)chunk.size);
        return chunks_.emplace_back(std::move(chunk));
    }

    uint64_t Context::Submit(plume::RenderCommandList& list)
    {
        plume::RenderCommandFence* fence;
        {
            std::lock_guard lock(mutex_);
            if (freeFences_.empty())
            {
                fences_.push_back(device_->createCommandFence());
                freeFences_.push_back(fences_.back().get());
            }
            fence = freeFences_.back();
            freeFences_.pop_back();
        }
        const plume::RenderCommandList* lists[] = { &list };
        queue_->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence);
        Push({ fence, false, 0 });
        return ++submitted_;
    }

    void Context::WaitSubmission(uint64_t submission)
    {
        std::unique_lock lock(mutex_);
        completedCv_.wait(lock, [&] { return completed_ >= submission; });
    }

    uint64_t Context::Completed()
    {
        std::lock_guard lock(mutex_);
        return completed_;
    }

    void Context::Push(const Pending& entry)
    {
        {
            std::lock_guard lock(mutex_);
            pending_.push_back(entry);
        }
        pendingCv_.notify_one();
    }

    void Context::CompleterMain()
    {
        for (;;)
        {
            Pending entry;
            {
                std::unique_lock lock(mutex_);
                pendingCv_.wait(lock, [&] { return stopping_ || !pending_.empty(); });
                if (pending_.empty())
                    return;
                entry = pending_.front();
                pending_.pop_front();
            }
            if (entry.fence != nullptr)
            {
                queue_->waitForCommandFence(entry.fence);
                {
                    std::lock_guard lock(mutex_);
                    freeFences_.push_back(entry.fence);
                    completed_++;
                }
                completedCv_.notify_all();
            }
            if (entry.hasToken)
                completion_(entry.token);
        }
    }
}
