// Guest threads: host threads running recompiled code with their own guest stack and KPCR.

#include "kernel/kernel.h"

#include "host/quit.h"
#include "loader.h"

#include <atomic>
#include <bit>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>

namespace kernel
{
    namespace
    {
        struct HostThread : GuestThread
        {
            std::mutex suspendMutex;
            std::condition_variable suspendCv;
            // What the thread runs once its stack and KPCR are in ctx: a guest entry point, or
            // host code for system threads.
            std::function<void(PPCContext&)> body;
        };

        std::mutex g_threadsMutex;
        std::vector<std::unique_ptr<HostThread>> g_threads; // threads are never freed
        std::atomic<uint32_t> g_nextThreadId{ 1 };
        thread_local HostThread* t_current = nullptr;
        std::atomic<GuestThread*> g_mainThread{ nullptr };

        std::mutex g_tlsMutex;
        uint64_t g_tlsUsed = 0;

        constexpr uint32_t kDispatcherThread = 6;
        constexpr uint32_t kPcrProcessor = 0x10C;
        constexpr uint32_t kHardwareThreads = 6;

        // Processor (hardware thread) of the calling guest thread: a new thread without an
        // affinity starts on the processor of the thread that creates it.
        uint8_t CurrentProcessor()
        {
            HostThread* thread = t_current;
            return thread != nullptr ? mem::Load8(thread->pcr + kPcrProcessor) : 0;
        }

        // The lowest hardware thread of an affinity mask, or fallback when the mask names none.
        uint8_t ProcessorFromAffinity(uint32_t affinity, uint8_t fallback)
        {
            affinity &= (1u << kHardwareThreads) - 1;
            return affinity != 0 ? uint8_t(std::countr_zero(affinity)) : fallback;
        }

        // KPCR+0x10C holds the processor number. Each guest thread has its own KPCR here, so the
        // number follows the thread: XAudio's processing threads index per-processor state with
        // it and wait for the processors they were created on.
        HostThread* NewThread(uint32_t stackSize, uint8_t processor)
        {
            auto thread = std::make_unique<HostThread>();
            thread->id = g_nextThreadId++;

            stackSize = std::max<uint32_t>(stackSize, 0x10000);
            stackSize = (stackSize + 0xFFFF) & ~0xFFFFu;
            thread->stackBase = mem::Alloc(mem::Heap::Stack, stackSize, 0x10000);
            thread->stackSize = stackSize;
            if (thread->stackBase == 0)
                Fatal("out of guest stack space");

            thread->object = std::make_shared<KernelObject>(ObjectType::Thread);
            AttachGuestObject(thread->object, kDispatcherThread);
            thread->kthread = thread->object->guestObject;

            // KPCR, the block r13 points to.
            thread->pcr = mem::Alloc(mem::Heap::System, 0x1000, 0x1000);
            uint32_t tlsBlock = mem::Alloc(mem::Heap::System, 0x400);
            uint32_t stackTop = thread->stackBase + stackSize;
            mem::Store32(thread->pcr + 0x00, tlsBlock);
            mem::Store32(thread->pcr + 0x30, thread->pcr);
            mem::Store32(thread->pcr + 0x70, stackTop);
            mem::Store32(thread->pcr + 0x74, thread->stackBase);
            mem::Store32(thread->pcr + 0x100, thread->kthread);
            mem::Store8(thread->pcr + kPcrProcessor, processor);

            // KTHREAD fields read by XAPI.
            mem::Store32(thread->kthread + 0x5C, stackTop);
            mem::Store32(thread->kthread + 0x60, thread->stackBase);
            mem::Store32(thread->kthread + 0x14C, thread->id);

            std::lock_guard lock(g_threadsMutex);
            g_threads.push_back(std::move(thread));
            return g_threads.back().get();
        }

        void RunGuest(HostThread* thread)
        {
            t_current = thread;
            PPCContext ctx{};
            ctx.r1.u64 = thread->stackBase + thread->stackSize - 0x100;
            ctx.r13.u64 = thread->pcr;
            ctx.fpscr.loadFromHost();
            thread->body(ctx);
        }

        // A body that calls a guest entry point with r3 and r4.
        std::function<void(PPCContext&)> GuestEntry(PPCFunc* entry, uint32_t arg0, uint32_t arg1)
        {
            return [=](PPCContext& ctx)
            {
                ctx.r3.u64 = arg0;
                ctx.r4.u64 = arg1;
                entry(ctx, mem::g_base);
            };
        }

        DWORD WINAPI HostThreadMain(void* parameter)
        {
            auto* thread = static_cast<HostThread*>(parameter);
            {
                std::unique_lock lock(thread->suspendMutex);
                thread->suspendCv.wait(lock, [&] { return thread->suspendCount <= 0; });
            }
            RunGuest(thread);
            SignalObject(thread->object);
            return 0;
        }

        void StartHostThread(HostThread* thread)
        {
            HANDLE host = CreateThread(nullptr, 16 * 1024 * 1024, HostThreadMain, thread, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
            if (host == nullptr)
                Fatal("cannot create the host thread of guest thread %u", thread->id);
            ::CloseHandle(host);
        }
    }

    GuestThread* CurrentThread()
    {
        return t_current;
    }

    GuestThread* ThreadById(uint32_t id)
    {
        std::lock_guard lock(g_threadsMutex);
        for (auto& thread : g_threads)
        {
            if (thread->id == id)
                return thread.get();
        }
        return nullptr;
    }

    GuestThread* ThreadByObject(uint32_t kthread)
    {
        std::lock_guard lock(g_threadsMutex);
        for (auto& thread : g_threads)
        {
            if (thread->kthread == kthread)
                return thread.get();
        }
        return nullptr;
    }

    GuestThread* MainThread()
    {
        return g_mainThread.load(std::memory_order_acquire);
    }

    std::shared_ptr<KernelObject> ThreadObject(GuestThread* thread)
    {
        return thread != nullptr ? thread->object : nullptr;
    }

    void RunMainThread(PPCFunc* entry, uint32_t stackSize)
    {
        HostThread* thread = NewThread(stackSize, 0);
        thread->name = "main";
        thread->body = GuestEntry(entry, 0, 0);
        g_mainThread.store(thread, std::memory_order_release);
        RunGuest(thread);
    }

    GuestThread* StartSystemThread(const char* name, std::function<void(PPCContext&)> body)
    {
        HostThread* thread = NewThread(g_module.stackSize, CurrentProcessor());
        thread->name = name;
        thread->body = std::move(body);
        StartHostThread(thread);
        return thread;
    }

    int32_t ResumeThread(GuestThread* guestThread)
    {
        auto* thread = static_cast<HostThread*>(guestThread);
        std::lock_guard lock(thread->suspendMutex);
        int32_t previous = thread->suspendCount;
        if (thread->suspendCount > 0)
            thread->suspendCount--;
        thread->suspendCv.notify_all();
        return previous;
    }
}

using namespace kernel;

void ReportBadIndirectCall(uint32_t target, PPCContext& ctx)
{
    Fatal("fatal: indirect call to %08X, which is not a known function (lr=%08X, thread %u)",
        target, ctx.lr, CurrentThread() ? CurrentThread()->id : 0);
}

// ExCreateThread(handle*, stackSize, threadId*, xapiStartup, startAddress, startContext, flags)
GUEST_FUNC(ExCreateThread)
{
    uint32_t handlePtr = ctx.r3.u32;
    uint32_t stackSize = ctx.r4.u32;
    uint32_t idPtr = ctx.r5.u32;
    uint32_t xapiStartup = ctx.r6.u32;
    uint32_t startAddress = ctx.r7.u32;
    uint32_t startContext = ctx.r8.u32;
    uint32_t flags = ctx.r9.u32;

    // With an XAPI startup routine, it runs first and calls startAddress(startContext) itself.
    uint32_t entry = xapiStartup != 0 ? xapiStartup : startAddress;
    PPCFunc* function = FindGuestFunction(entry);
    if (function == nullptr)
        Fatal("ExCreateThread: entry %08X is not a known function", entry);

    // Bits 24-29 of the flags are the affinity (XAudio: (1 << hardwareThread) << 24 | suspended).
    uint8_t processor = ProcessorFromAffinity(flags >> 24, CurrentProcessor());
    auto* thread = NewThread(stackSize != 0 ? stackSize : g_module.stackSize, processor);
    thread->body = GuestEntry(function, xapiStartup != 0 ? startAddress : startContext,
        xapiStartup != 0 ? startContext : 0);
    thread->suspendCount = (flags & 1) ? 1 : 0; // CREATE_SUSPENDED

    TRACE("ExCreateThread: thread %u entry %08X start %08X context %08X flags %08X processor %u",
        thread->id, entry, startAddress, startContext, flags, processor);

    StartHostThread(thread);

    if (handlePtr != 0)
        mem::Store32(handlePtr, CreateHandle(thread->object));
    if (idPtr != 0)
        mem::Store32(idPtr, thread->id);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

GUEST_FUNC(ExTerminateThread)
{
    GuestThread* thread = CurrentThread();
    TRACE("ExTerminateThread: thread %u, code %08X", thread ? thread->id : 0, ctx.r3.u32);
    if (thread == nullptr || thread->name == "main")
    {
        Log("main thread terminated (code %08X)", ctx.r3.u32);
        host::QuitFromGuest("the game's main thread ended");
    }
    SignalObject(thread->object);
    ExitThread(ctx.r3.u32);
}

// NtResumeThread(handle, previousSuspendCount*)
GUEST_FUNC(NtResumeThread)
{
    auto object = ObjectFromHandle(ctx.r3.u32);
    GuestThread* thread = object ? ThreadByObject(object->guestObject) : nullptr;
    if (thread == nullptr)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }
    int32_t previous = ResumeThread(thread);
    if (ctx.r4.u32 != 0)
        mem::Store32(ctx.r4.u32, uint32_t(previous));
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

GUEST_FUNC(KeResumeThread)
{
    GuestThread* thread = ThreadByObject(ctx.r3.u32);
    ctx.r3.u64 = thread != nullptr ? uint32_t(ResumeThread(thread)) : 0;
}

GUEST_FUNC(NtSuspendThread)
{
    Log("warning: NtSuspendThread(%08X) ignored", ctx.r3.u32);
    if (ctx.r4.u32 != 0)
        mem::Store32(ctx.r4.u32, 0);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// KeSetAffinityThread(thread, affinity, previousAffinity*): moves the thread to the lowest hardware
// thread of the mask (XSetThreadProcessor passes a single bit). The thread may be running: the
// processor byte is read by its own code, which sees the new value from then on.
GUEST_FUNC(KeSetAffinityThread)
{
    GuestThread* thread = ThreadByObject(ctx.r3.u32);
    uint32_t affinity = ctx.r4.u32 & ((1u << kHardwareThreads) - 1);
    if (thread == nullptr || affinity == 0)
    {
        ctx.r3.u64 = X_STATUS_INVALID_PARAMETER;
        return;
    }
    uint8_t previous = mem::Load8(thread->pcr + kPcrProcessor);
    uint8_t processor = ProcessorFromAffinity(affinity, previous);
    mem::Store8(thread->pcr + kPcrProcessor, processor);
    if (ctx.r5.u32 != 0)
        mem::Store32(ctx.r5.u32, 1u << previous);
    TRACE("KeSetAffinityThread: thread %u, processor %u -> %u", thread->id, previous, processor);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

GUEST_FUNC(KeSetBasePriorityThread) { ctx.r3.u64 = 0; }
GUEST_FUNC(KeSetDisableBoostThread) { ctx.r3.u64 = 0; }
GUEST_FUNC(KeGetCurrentProcessType) { ctx.r3.u64 = 1; } // title process

// KeDelayExecutionThread(processorMode, alertable, interval*)
GUEST_FUNC(KeDelayExecutionThread)
{
    int64_t interval = LoadLargeInteger(ctx.r5.u32);
    if (interval > 0)
        interval = std::max<int64_t>(0, interval - int64_t(SystemTime()));
    else
        interval = -interval;

    if (interval == 0)
        std::this_thread::yield();
    else
        std::this_thread::sleep_for(std::chrono::nanoseconds(interval * 100));
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

GUEST_FUNC(NtYieldExecution)
{
    std::this_thread::yield();
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// ---- TLS ----

GUEST_FUNC(KeTlsAlloc)
{
    std::lock_guard lock(g_tlsMutex);
    for (uint32_t i = 0; i < 64; i++)
    {
        if (!(g_tlsUsed & (1ull << i)))
        {
            g_tlsUsed |= 1ull << i;
            ctx.r3.u64 = i;
            return;
        }
    }
    ctx.r3.u64 = 0xFFFFFFFF;
}

GUEST_FUNC(KeTlsFree)
{
    std::lock_guard lock(g_tlsMutex);
    if (ctx.r3.u32 < 64)
        g_tlsUsed &= ~(1ull << ctx.r3.u32);
    ctx.r3.u64 = 1;
}

GUEST_FUNC(KeTlsGetValue)
{
    GuestThread* thread = CurrentThread();
    ctx.r3.u64 = (thread != nullptr && ctx.r3.u32 < 64) ? thread->tls[ctx.r3.u32] : 0;
}

GUEST_FUNC(KeTlsSetValue)
{
    GuestThread* thread = CurrentThread();
    bool ok = thread != nullptr && ctx.r3.u32 < 64;
    if (ok)
        thread->tls[ctx.r3.u32] = ctx.r4.u32;
    ctx.r3.u64 = ok ? 1 : 0;
}

// ---- Objects ----

// ObReferenceObjectByHandle(handle, objectType, object*)
GUEST_FUNC(ObReferenceObjectByHandle)
{
    auto object = ObjectFromHandle(ctx.r3.u32);
    if (object == nullptr)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }
    if (object->guestObject == 0)
        AttachGuestObject(object, 0);
    if (ctx.r5.u32 != 0)
        mem::Store32(ctx.r5.u32, object->guestObject);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

GUEST_FUNC(ObReferenceObject) {}
GUEST_FUNC(ObDereferenceObject) {}
GUEST_FUNC(ObIsTitleObject) { ctx.r3.u64 = 1; }

// ObLookupThreadByThreadId(threadId, thread*)
GUEST_FUNC(ObLookupThreadByThreadId)
{
    GuestThread* thread = ThreadById(ctx.r3.u32);
    if (thread == nullptr)
    {
        ctx.r3.u64 = X_STATUS_INVALID_PARAMETER;
        return;
    }
    mem::Store32(ctx.r4.u32, thread->kthread);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// ObOpenObjectByPointer(object, handle*)
GUEST_FUNC(ObOpenObjectByPointer)
{
    auto object = ObjectFromGuest(ctx.r3.u32);
    mem::Store32(ctx.r4.u32, HandleForObject(object));
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

GUEST_FUNC(NtClose)
{
    ctx.r3.u64 = CloseHandle(ctx.r3.u32) ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE;
}

// NtDuplicateObject(handle, newHandle*, options)
GUEST_FUNC(NtDuplicateObject)
{
    auto object = ObjectFromHandle(ctx.r3.u32);
    if (object == nullptr)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }
    if (ctx.r4.u32 != 0)
        mem::Store32(ctx.r4.u32, CreateHandle(object));
    if (ctx.r5.u32 & 1) // DUPLICATE_CLOSE_SOURCE
        CloseHandle(ctx.r3.u32);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}
