// The renderer: device creation and the owner thread that runs the posted operations.

#include "gpu/renderer/renderer.h"

#include "gpu/renderer/buffers.h"
#include "gpu/renderer/constant_ring.h"
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

#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <iterator>
#include <memory>
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
        // The draws the owner receives together.
        constexpr size_t kDrawBatch = 32;

        // The constant ring of each stage. A frame of the heaviest scene seen (4,000 draws) hands out
        // about 2 MB of vertex constants and 0.6 MB of pixel constants, and the capture runs at most
        // about two frames ahead of the owner.
        constexpr uint64_t kConstantRingBytes = 8 << 20;

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
                    CloseDraws();
                    stopping_ = true;
                }
                cv_.notify_all();
                roomCv_.notify_all();
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

        // False when the owner is stopping: the task is dropped. A task queued before the stop runs,
        // after the draws posted before it.
        bool Post(std::function<void()> task)
        {
            beforePost();
            {
                std::lock_guard lock(mutex_);
                if (stopping_)
                    return false;
                CloseDraws();
                tasks_.push_back({ std::move(task), {} });
            }
            cv_.notify_one();
            return true;
        }

        std::unique_ptr<DrawOp> NewDraw()
        {
            {
                std::lock_guard lock(mutex_);
                if (!freeDraws_.empty())
                {
                    std::unique_ptr<DrawOp> op = std::move(freeDraws_.back());
                    freeDraws_.pop_back();
                    return op;
                }
            }
            return std::make_unique<DrawOp>();
        }

        // The draw joins the open batch, which is queued once it is full or before the next task. The
        // draw is dropped when the owner is stopping.
        void Draw(std::unique_ptr<DrawOp> op)
        {
            beforePost();
            {
                std::lock_guard lock(mutex_);
                if (stopping_)
                    return;
                if (openDraws_.empty())
                    openDraws_.reserve(kDrawBatch);
                openDraws_.push_back(std::move(op));
                if (openDraws_.size() < kDrawBatch)
                    return;
                CloseDraws();
            }
            cv_.notify_one();
        }

        std::optional<ConstantSpace> ReserveConstants(ShaderStage stage, uint32_t bytes)
        {
            ConstantRing& ring = Ring(stage);
            for (;;)
            {
                // Read before the attempt, so that a free after it ends the wait below.
                uint64_t freed = ring.Freed();
                if (std::optional<ConstantSpace> space = ring.TryReserve(bytes))
                    return space;
                LOG_ONCE("gpu: a constant ring is full; the capture waits for the owner");
                std::unique_lock lock(mutex_);
                if (stopping_)
                    return std::nullopt;
                // The open batch may hold the draws whose drawing frees the space.
                CloseDraws();
                cv_.notify_one();
                roomCv_.wait(lock, [&] { return stopping_ || ring.Freed() != freed; });
            }
        }

        // Destroyed in reverse order: the device outlives everything created on it, and the constant
        // rings outlive the draws that read them.
        ConstantRing vertexRing{ kConstantRingBytes };
        ConstantRing pixelRing{ kConstantRingBytes };
        std::unique_ptr<plume::RenderInterface> api;
        std::unique_ptr<Context> context;
        std::unique_ptr<Descriptors> descriptors;
        std::unique_ptr<Passes> passes;
        std::unique_ptr<Targets> targets;
        std::unique_ptr<Textures> textures;
        std::unique_ptr<Buffers> buffers;
        std::unique_ptr<Draws> draws;
        std::unique_ptr<Presenter> presenter;
        Renderer::BeforePost beforePost;

        // Presents: numbered as they are posted, and the last one the owner handed to the swap chain.
        std::atomic<uint64_t> presentsPosted{ 0 };
        std::mutex presentMutex;
        std::condition_variable presentDone;
        uint64_t presentsDone = 0; // presentMutex
        bool lastPaced = false;    // presentMutex
        std::atomic<bool> displayPaces{ false };

    private:
        using DrawBatch = std::vector<std::unique_ptr<DrawOp>>;

        // What the owner runs: an operation, or the draws of a batch in posting order.
        struct Task
        {
            std::function<void()> run;
            DrawBatch draws;
        };

        ConstantRing& Ring(ShaderStage stage)
        {
            return stage == ShaderStage::Vertex ? vertexRing : pixelRing;
        }

        // Queues the open batch of draws, if there is one. Under mutex_.
        void CloseDraws()
        {
            if (openDraws_.empty())
                return;
            tasks_.push_back({ {}, std::move(openDraws_) });
        }

        void OwnerMain()
        {
            DrawBatch drawn; // the draws of the last batch, on their way to the free ones
            // The ranges of the last draw drawn: the space before them is free once the batch is.
            std::optional<std::pair<uint64_t, uint64_t>> constantsFree;
            for (;;)
            {
                Task task;
                {
                    std::unique_lock lock(mutex_);
                    std::move(drawn.begin(), drawn.end(), std::back_inserter(freeDraws_));
                    drawn.clear();
                    // Under the lock, so that a capture waiting for room cannot miss it.
                    if (constantsFree)
                    {
                        vertexRing.Free(constantsFree->first);
                        pixelRing.Free(constantsFree->second);
                        constantsFree.reset();
                        roomCv_.notify_all();
                    }
                    cv_.wait(lock, [&] { return stopping_ || !tasks_.empty(); });
                    if (tasks_.empty())
                        return;
                    task = std::move(tasks_.front());
                    tasks_.pop_front();
                }
                if (task.run)
                {
                    task.run();
                    continue;
                }
                const DrawOp& last = *task.draws.back();
                constantsFree.emplace(last.vertexConstants.position, last.pixelConstants.position);
                for (std::unique_ptr<DrawOp>& op : task.draws)
                {
                    draws->Draw(*op);
                    // A new draw in the same memory, for NewDraw.
                    std::destroy_at(op.get());
                    std::construct_at(op.get());
                }
                drawn = std::move(task.draws);
            }
        }

        std::thread owner_;
        std::mutex mutex_; // the state below
        std::condition_variable cv_;
        std::condition_variable roomCv_; // a constant ring was freed
        std::deque<Task> tasks_;
        DrawBatch openDraws_; // posted since the last task
        // Drawn DrawOps for NewDraw: as many as were ever queued at once, about two frames of draws
        // (the title waits for the previous frame before it ends one).
        DrawBatch freeDraws_;
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
        impl->buffers = std::make_unique<Buffers>(*impl->context);
        impl->draws = std::make_unique<Draws>(*impl->context, *impl->targets, *impl->descriptors, *impl->textures,
            *impl->buffers, impl->vertexRing, impl->pixelRing, settings.pipelineList);
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

    std::unique_ptr<DrawOp> Renderer::NewDraw()
    {
        return impl_->NewDraw();
    }

    void Renderer::Draw(std::unique_ptr<DrawOp> op)
    {
        impl_->Draw(std::move(op));
    }

    std::optional<ConstantSpace> Renderer::ReserveConstants(ShaderStage stage, uint32_t bytes)
    {
        return impl_->ReserveConstants(stage, bytes);
    }

    void Renderer::UploadTexture(std::unique_ptr<TextureUpload> upload)
    {
        impl_->Post([impl = impl_.get(), upload = std::shared_ptr<TextureUpload>(std::move(upload))] {
            impl->textures->Upload(*upload);
        });
    }

    void Renderer::UploadBuffer(std::unique_ptr<BufferUpload> upload)
    {
        impl_->Post([impl = impl_.get(), upload = std::shared_ptr<BufferUpload>(std::move(upload))] {
            impl->buffers->Upload(*upload);
        });
    }

    void Renderer::ReleaseBuffers(uint32_t begin, uint32_t end)
    {
        impl_->Post([impl = impl_.get(), begin, end] { impl->buffers->Release(begin, end); });
    }

    void Renderer::ReleaseBuffer(const BufferKey& key)
    {
        impl_->Post([impl = impl_.get(), key] { impl->buffers->Forget(key); });
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

    uint64_t Renderer::Present(uint32_t frontAddress, uint32_t vblanks, uint32_t refreshMillihertz)
    {
        uint64_t present = impl_->presentsPosted.fetch_add(1) + 1;
        if (!impl_->Post([impl = impl_.get(), frontAddress, vblanks, refreshMillihertz, present] {
                impl->presenter->Present(*impl->passes, impl->targets->Resolved(frontAddress), vblanks, refreshMillihertz,
                    [&](bool paced) {
                        impl->displayPaces = paced;
                        {
                            std::lock_guard lock(impl->presentMutex);
                            if (present > impl->presentsDone)
                            {
                                impl->presentsDone = present;
                                impl->lastPaced = paced;
                            }
                        }
                        impl->presentDone.notify_all();
                    });
            }))
            return 0;
        return present;
    }

    bool Renderer::WaitPresented(uint64_t present)
    {
        // A posted task always runs (the owner drains its queue before stopping).
        std::unique_lock lock(impl_->presentMutex);
        impl_->presentDone.wait(lock, [&] { return impl_->presentsDone >= present; });
        return impl_->presentsDone == present && impl_->lastPaced;
    }

    bool Renderer::DisplayPaces() const
    {
        return impl_->displayPaces;
    }

    void Renderer::SetGammaRamp(const GammaRamp& ramp)
    {
        impl_->Post([impl = impl_.get(), ramp] { impl->presenter->SetGammaRamp(ramp); });
    }
}
