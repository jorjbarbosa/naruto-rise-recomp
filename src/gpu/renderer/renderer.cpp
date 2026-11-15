// The renderer: device creation and the owner thread that runs the posted operations.

#include "gpu/renderer/renderer.h"

#include "gpu/renderer/context.h"
#include "gpu/renderer/descriptors.h"
#include "gpu/renderer/draws.h"
#include "gpu/renderer/passes.h"
#include "gpu/renderer/presenter.h"
#include "gpu/renderer/targets.h"
#include "gpu/renderer/textures.h"
#include "gpu/settings.h"
#include "log.h"

#include <plume_d3d12.h>

#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

namespace plume
{
    std::unique_ptr<RenderInterface> CreateD3D12Interface();
}

namespace gpu
{
    namespace
    {
        // Debug layer: the guest clears to any value, so the hint that a clear does not match the
        // value given at the target's creation is noise.
        void FilterDebugMessages(plume::RenderDevice& device)
        {
            ID3D12InfoQueue* queue = nullptr;
            if (FAILED(static_cast<plume::D3D12Device&>(device).d3d->QueryInterface(IID_PPV_ARGS(&queue))))
                return;
            D3D12_MESSAGE_ID ignored[] = { D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
                D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE };
            D3D12_INFO_QUEUE_FILTER filter = {};
            filter.DenyList.NumIDs = UINT(std::size(ignored));
            filter.DenyList.pIDList = ignored;
            queue->AddStorageFilterEntries(&filter);
            queue->Release();
        }
    }

    class Renderer::Impl
    {
    public:
        ~Impl()
        {
            if (owner_.joinable())
            {
                {
                    std::lock_guard lock(mutex_);
                    stopping_ = true;
                }
                cv_.notify_all();
                owner_.join();
            }
            if (context)
                context->WaitIdle();
        }

        void Start()
        {
            owner_ = std::thread([this] { OwnerMain(); });
            SetThreadDescription(static_cast<HANDLE>(owner_.native_handle()), L"gpu owner");
        }

        // False when the owner is stopping: the task is dropped. A task queued before the stop runs.
        bool Post(std::function<void()> task)
        {
            beforePost();
            {
                std::lock_guard lock(mutex_);
                if (stopping_)
                    return false;
                tasks_.push_back(std::move(task));
            }
            cv_.notify_one();
            return true;
        }

        // Destroyed in reverse order: the device outlives everything created on it.
        std::unique_ptr<plume::RenderInterface> api;
        std::unique_ptr<Context> context;
        std::unique_ptr<Descriptors> descriptors;
        std::unique_ptr<Passes> passes;
        std::unique_ptr<Targets> targets;
        std::unique_ptr<Textures> textures;
        std::unique_ptr<Draws> draws;
        std::unique_ptr<Presenter> presenter;
        Renderer::BeforePost beforePost;

    private:
        void OwnerMain()
        {
            for (;;)
            {
                std::function<void()> task;
                {
                    std::unique_lock lock(mutex_);
                    cv_.wait(lock, [&] { return stopping_ || !tasks_.empty(); });
                    if (tasks_.empty())
                        return;
                    task = std::move(tasks_.front());
                    tasks_.pop_front();
                }
                task();
            }
        }

        std::thread owner_;
        std::mutex mutex_;
        std::condition_variable cv_;
        std::deque<std::function<void()>> tasks_;
        bool stopping_ = false;
    };

    std::unique_ptr<Renderer> Renderer::Create(void* window, const Settings& settings, Completion completion,
        BeforePost beforePost)
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

        auto impl = std::make_unique<Impl>();
        impl->beforePost = std::move(beforePost);
        impl->api = plume::CreateD3D12Interface();
        if (!impl->api)
        {
            Log("gpu: error: cannot create the D3D12 interface");
            return nullptr;
        }
        for (const std::string& name : impl->api->getDeviceNames())
            Log("gpu: adapter \"%s\"", name.c_str());

        std::unique_ptr<plume::RenderDevice> device = impl->api->createDevice(settings.adapter);
        if (!device)
        {
            Log("gpu: error: cannot create the D3D12 device");
            return nullptr;
        }
        const auto& description = device->getDescription();
        Log("gpu: device \"%s\", %llu MB", description.name.c_str(),
            (unsigned long long)(description.dedicatedVideoMemory >> 20));
        if (settings.debugLayer)
            FilterDebugMessages(*device);

        impl->context = std::make_unique<Context>(std::move(device), std::move(completion));
        impl->presenter = Presenter::Create(*impl->context, window, settings);
        if (!impl->presenter)
            return nullptr;
        impl->descriptors = std::make_unique<Descriptors>(impl->context->Device());
        impl->passes = std::make_unique<Passes>(impl->context->Device(), Presenter::kFormat);
        impl->targets = std::make_unique<Targets>(*impl->context, *impl->passes, *impl->descriptors);
        impl->textures = std::make_unique<Textures>(*impl->context, *impl->targets, *impl->descriptors);
        impl->draws = std::make_unique<Draws>(*impl->context, *impl->targets, *impl->descriptors, *impl->textures,
            settings.pipelineList);
        impl->Start();
        return std::unique_ptr<Renderer>(new Renderer(std::move(impl)));
    }

    Renderer::Renderer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

    Renderer::~Renderer() = default;

    void Renderer::Kick(uint32_t token)
    {
        impl_->Post([impl = impl_.get(), token] { impl->context->Report(token); });
    }

    void Renderer::Clear(const ClearOp& op)
    {
        impl_->Post([impl = impl_.get(), op] { impl->targets->Clear(op); });
    }

    void Renderer::Resolve(const ResolveOp& op)
    {
        impl_->Post([impl = impl_.get(), op] { impl->targets->Resolve(op); });
    }

    void Renderer::Draw(std::unique_ptr<DrawOp> op)
    {
        // std::function needs a copyable task.
        impl_->Post([impl = impl_.get(), op = std::shared_ptr<DrawOp>(std::move(op))] { impl->draws->Draw(*op); });
    }

    void Renderer::UploadTexture(std::unique_ptr<TextureUpload> upload)
    {
        impl_->Post([impl = impl_.get(), upload = std::shared_ptr<TextureUpload>(std::move(upload))] {
            impl->textures->Upload(*upload);
        });
    }

    void Renderer::ReleaseSurface(uint32_t key)
    {
        impl_->Post([impl = impl_.get(), key] { impl->targets->ReleaseSurface(key); });
    }

    void Renderer::ReleaseTexture(uint32_t address, uint32_t header)
    {
        impl_->Post([impl = impl_.get(), address, header] {
            impl->targets->ReleaseTexture(address, header);
            impl->textures->Release(address, address + 1);
        });
    }

    void Renderer::ReleaseMemory(uint32_t begin, uint32_t end)
    {
        impl_->Post([impl = impl_.get(), begin, end] {
            impl->targets->ReleaseResolved(begin, end);
            impl->textures->Release(begin, end);
        });
    }

    bool Renderer::ReadResolved(uint32_t address, ResolvedImage& image)
    {
        // The task runs once queued (the owner drains its queue before stopping), so the promise is
        // always set and get() cannot throw.
        std::promise<bool> done;
        std::future<bool> result = done.get_future();
        if (!impl_->Post([impl = impl_.get(), address, &image, &done] {
                done.set_value(impl->targets->ReadResolved(address, image));
            }))
            return false;
        return result.get();
    }

    void Renderer::Present(uint32_t frontAddress)
    {
        impl_->Post([impl = impl_.get(), frontAddress] {
            impl->presenter->Present(*impl->passes, impl->targets->Resolved(frontAddress));
        });
    }
}
