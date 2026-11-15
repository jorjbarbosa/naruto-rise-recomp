// D3D12 renderer on Plume: owner thread, completion thread and present.

#include "gpu/renderer/renderer.h"

#include "gpu/settings.h"
#include "log.h"

#include <plume_d3d12.h>

#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace plume
{
    std::unique_ptr<RenderInterface> CreateD3D12Interface();
}

namespace gpu
{
    namespace
    {
        constexpr uint32_t kSwapBuffers = 3;
        constexpr uint32_t kFrameLatency = 1;
        constexpr uint32_t kFramesInFlight = 2;
        constexpr plume::RenderFormat kSwapFormat = plume::RenderFormat::B8G8R8A8_UNORM;
        // Placeholder image until the guest front buffer is presented.
        const plume::RenderColor kClearColor(0.0f, 0.25f, 0.5f, 1.0f);

        void NameThread(std::thread& thread, const wchar_t* name)
        {
            SetThreadDescription(static_cast<HANDLE>(thread.native_handle()), name);
        }
    }

    struct Renderer::State
    {
        Completion completion;

        std::unique_ptr<plume::RenderInterface> api;
        std::unique_ptr<plume::RenderDevice> device;
        std::unique_ptr<plume::RenderCommandQueue> queue;
        std::unique_ptr<plume::RenderSwapChain> swapChain;
        std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffers;

        struct Frame
        {
            std::unique_ptr<plume::RenderCommandList> list;
            uint64_t submission = 0; // submission that last used the list
        };
        std::array<Frame, kFramesInFlight> frames;
        uint32_t nextFrame = 0;
        // Empty list submitted to fence the queue after the last Present.
        std::unique_ptr<plume::RenderCommandList> idleList;
        uint64_t submitted = 0; // owner thread
        bool presentFailed = false;

        // Owner thread and its task queue.
        std::thread owner;
        std::mutex taskMutex;
        std::condition_variable taskCv;
        std::deque<std::function<void()>> tasks;
        bool stopping = false;

        // Completion thread. A pending entry is a submission to wait for (fence) or a token to
        // report; the queue keeps posting order.
        struct Pending
        {
            plume::RenderCommandFence* fence = nullptr;
            bool hasToken = false;
            uint32_t token = 0;
        };
        std::thread completer;
        std::mutex pendingMutex;
        std::condition_variable pendingCv;
        std::condition_variable completedCv;
        std::deque<Pending> pending;
        std::vector<std::unique_ptr<plume::RenderCommandFence>> fences;
        std::vector<plume::RenderCommandFence*> freeFences;
        uint64_t completed = 0; // submissions the GPU has finished
        bool completerStopping = false;

        void Post(std::function<void()> task)
        {
            {
                std::lock_guard lock(taskMutex);
                tasks.push_back(std::move(task));
            }
            taskCv.notify_one();
        }

        void OwnerMain()
        {
            for (;;)
            {
                std::function<void()> task;
                {
                    std::unique_lock lock(taskMutex);
                    taskCv.wait(lock, [&] { return stopping || !tasks.empty(); });
                    if (tasks.empty())
                        return;
                    task = std::move(tasks.front());
                    tasks.pop_front();
                }
                task();
            }
        }

        void CompleterMain()
        {
            for (;;)
            {
                Pending entry;
                {
                    std::unique_lock lock(pendingMutex);
                    pendingCv.wait(lock, [&] { return completerStopping || !pending.empty(); });
                    if (pending.empty())
                        return;
                    entry = pending.front();
                    pending.pop_front();
                }
                if (entry.fence != nullptr)
                {
                    queue->waitForCommandFence(entry.fence);
                    {
                        std::lock_guard lock(pendingMutex);
                        freeFences.push_back(entry.fence);
                        completed++;
                    }
                    completedCv.notify_all();
                }
                if (entry.hasToken)
                    completion(entry.token);
            }
        }

        void PushPending(const Pending& entry)
        {
            {
                std::lock_guard lock(pendingMutex);
                pending.push_back(entry);
            }
            pendingCv.notify_one();
        }

        // Owner thread: submits a command list; returns its submission number.
        uint64_t Submit(plume::RenderCommandList* list)
        {
            plume::RenderCommandFence* fence;
            {
                std::lock_guard lock(pendingMutex);
                if (freeFences.empty())
                {
                    fences.push_back(device->createCommandFence());
                    freeFences.push_back(fences.back().get());
                }
                fence = freeFences.back();
                freeFences.pop_back();
            }
            const plume::RenderCommandList* lists[] = { list };
            queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence);
            PushPending({ fence, false, 0 });
            return ++submitted;
        }

        // Owner thread: waits until the GPU has finished the given submission.
        void WaitSubmission(uint64_t submission)
        {
            std::unique_lock lock(pendingMutex);
            completedCv.wait(lock, [&] { return completed >= submission; });
        }

        // Owner thread: waits until the queue is idle, Presents included (they follow the last
        // command list, so its fence does not cover them).
        void WaitIdle()
        {
            idleList->begin();
            idleList->end();
            WaitSubmission(Submit(idleList.get()));
        }

        void CreateFramebuffers()
        {
            framebuffers.clear();
            for (uint32_t i = 0; i < swapChain->getTextureCount(); i++)
            {
                const plume::RenderTexture* attachment = swapChain->getTexture(i);
                framebuffers.push_back(device->createFramebuffer(plume::RenderFramebufferDesc(&attachment, 1)));
            }
        }

        // Owner thread.
        void PresentClear()
        {
            if (swapChain->needsResize())
            {
                WaitIdle();
                framebuffers.clear();
                if (swapChain->resize())
                    CreateFramebuffers();
                else if (swapChain->getWidth() != 0 && swapChain->getHeight() != 0)
                    Log("gpu: swap chain resize to %ux%u failed", swapChain->getWidth(), swapChain->getHeight());
            }
            if (swapChain->isEmpty() || framebuffers.empty())
                return; // minimized

            // Frame latency: wait (bounded) until the swap chain queue has room for this frame.
            auto* native = static_cast<plume::D3D12SwapChain*>(swapChain.get());
            if (native->waitableObject != nullptr)
                WaitForSingleObject(native->waitableObject, 100);

            Frame& frame = frames[nextFrame];
            nextFrame = (nextFrame + 1) % kFramesInFlight;
            WaitSubmission(frame.submission);

            uint32_t index = 0;
            swapChain->acquireTexture(nullptr, &index);
            plume::RenderTexture* texture = swapChain->getTexture(index);
            plume::RenderCommandList* list = frame.list.get();
            list->begin();
            list->barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(texture, plume::RenderTextureLayout::COLOR_WRITE));
            list->setFramebuffer(framebuffers[index].get());
            list->clearColor(0, kClearColor);
            list->barriers(plume::RenderBarrierStage::NONE,
                plume::RenderTextureBarrier(texture, plume::RenderTextureLayout::PRESENT));
            list->end();
            frame.submission = Submit(list);

            if (!swapChain->present(index, nullptr, 0) && !presentFailed)
            {
                presentFailed = true;
                Log("gpu: Present failed");
            }
        }
    };

    std::unique_ptr<Renderer> Renderer::Create(void* window, const Settings& settings, Completion completion)
    {
        if (settings.debugLayer)
        {
            // Before Plume creates the device.
            ID3D12Debug* debug = nullptr;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            {
                debug->EnableDebugLayer();
                debug->Release();
                Log("gpu: D3D12 debug layer on");
            }
            else
            {
                Log("gpu: D3D12 debug layer unavailable");
            }
        }

        auto state = std::make_unique<State>();
        state->completion = std::move(completion);
        state->api = plume::CreateD3D12Interface();
        if (!state->api)
        {
            Log("gpu: error: cannot create the D3D12 interface");
            return nullptr;
        }
        for (const std::string& name : state->api->getDeviceNames())
            Log("gpu: adapter \"%s\"", name.c_str());

        state->device = state->api->createDevice(settings.adapter);
        if (!state->device)
        {
            Log("gpu: error: cannot create the D3D12 device");
            return nullptr;
        }
        const auto& description = state->device->getDescription();
        Log("gpu: device \"%s\", %llu MB", description.name.c_str(),
            (unsigned long long)(description.dedicatedVideoMemory >> 20));

        state->queue = state->device->createCommandQueue(plume::RenderCommandListType::DIRECT);
        state->swapChain = state->queue->createSwapChain(plume::RenderSwapChainDesc(
            static_cast<plume::RenderWindow>(window), kSwapFormat, kSwapBuffers, false, kFrameLatency));
        if (static_cast<plume::D3D12SwapChain*>(state->swapChain.get())->d3d == nullptr)
        {
            Log("gpu: error: cannot create the swap chain");
            return nullptr;
        }
        state->swapChain->setVsyncEnabled(settings.vsync);
        state->CreateFramebuffers();
        for (auto& frame : state->frames)
            frame.list = state->queue->createCommandList();
        state->idleList = state->queue->createCommandList();

        state->owner = std::thread([s = state.get()] { s->OwnerMain(); });
        state->completer = std::thread([s = state.get()] { s->CompleterMain(); });
        NameThread(state->owner, L"gpu owner");
        NameThread(state->completer, L"gpu completion");
        Log("gpu: swap chain %ux%u, vsync %s", state->swapChain->getWidth(), state->swapChain->getHeight(),
            settings.vsync ? "on" : "off");
        return std::unique_ptr<Renderer>(new Renderer(std::move(state)));
    }

    Renderer::Renderer(std::unique_ptr<State> state) : state_(std::move(state)) {}

    Renderer::~Renderer()
    {
        {
            std::lock_guard lock(state_->taskMutex);
            state_->stopping = true;
        }
        state_->taskCv.notify_all();
        state_->owner.join();
        {
            std::lock_guard lock(state_->pendingMutex);
            state_->completerStopping = true;
        }
        state_->pendingCv.notify_all();
        state_->completer.join();
    }

    void Renderer::Kick(uint32_t token)
    {
        // Through the owner thread, so the token follows every submission posted before it.
        state_->Post([s = state_.get(), token] { s->PushPending({ nullptr, true, token }); });
    }

    void Renderer::Present()
    {
        state_->Post([s = state_.get()] { s->PresentClear(); });
    }
}
