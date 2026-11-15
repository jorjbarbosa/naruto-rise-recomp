// Events, semaphores, mutants, timers, waits, critical sections and spinlocks.

#include "kernel/kernel.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

using namespace kernel;

namespace
{
    constexpr int kSynchronizationEvent = 1;
    constexpr uint32_t kDispatcherMutant = 2;
    constexpr uint32_t kDispatcherSemaphore = 5;
    constexpr uint32_t kDispatcherTimer = 8;

    uint32_t StoreNewHandle(uint32_t handlePtr, const ObjectPtr& object)
    {
        uint32_t handle = CreateHandle(object);
        if (handlePtr != 0)
            mem::Store32(handlePtr, handle);
        return handle;
    }

    // Name from the guest OBJECT_ATTRIBUTES (root directory, ANSI_STRING* name, attributes).
    std::string ObjectName(uint32_t attributes)
    {
        if (attributes == 0)
            return {};
        uint32_t name = mem::Load32(attributes + 4);
        return name != 0 ? LoadAnsiString(name) : std::string();
    }

    // Ends an NtCreate* call: stores a handle to the object (new or shared by name) and the status.
    void FinishCreate(PPCContext& ctx, uint32_t handlePtr, const ObjectPtr& object, uint32_t status)
    {
        if (object != nullptr)
            StoreNewHandle(handlePtr, object);
        ctx.r3.u64 = status;
    }

    uint32_t CurrentKThread()
    {
        GuestThread* thread = CurrentThread();
        return thread != nullptr ? thread->kthread : 0;
    }
}

// ---- Events ----

// NtCreateEvent(handle*, objectAttributes, eventType, initialState)
GUEST_FUNC(NtCreateEvent)
{
    bool manualReset = ctx.r5.u32 == 0;
    bool initialState = ctx.r6.u32 != 0;
    uint32_t status;
    auto object = CreateNamedObject(ObjectName(ctx.r4.u32), ObjectType::Event,
        [&] { return CreateEvent(manualReset, initialState); }, status);
    FinishCreate(ctx, ctx.r3.u32, object, status);
}

// NtSetEvent(handle, previousState*)
GUEST_FUNC(NtSetEvent)
{
    auto object = ObjectFromHandle(ctx.r3.u32);
    if (object == nullptr)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }
    int32_t previous = SetEvent(object);
    if (ctx.r4.u32 != 0)
        mem::Store32(ctx.r4.u32, uint32_t(previous));
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

GUEST_FUNC(NtClearEvent)
{
    auto object = ObjectFromHandle(ctx.r3.u32);
    if (object == nullptr)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }
    ResetEvent(object);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// KeSetEvent(event, increment, wait) -> previous state
GUEST_FUNC(KeSetEvent)
{
    ctx.r3.u64 = uint32_t(SetEvent(ObjectFromGuest(ctx.r3.u32)));
}

GUEST_FUNC(KeResetEvent)
{
    ctx.r3.u64 = uint32_t(ResetEvent(ObjectFromGuest(ctx.r3.u32)));
}

// ---- Semaphores ----

// NtCreateSemaphore(handle*, objectAttributes, initialCount, maximumCount)
GUEST_FUNC(NtCreateSemaphore)
{
    uint32_t count = ctx.r5.u32;
    uint32_t limit = ctx.r6.u32;
    uint32_t status;
    auto object = CreateNamedObject(ObjectName(ctx.r4.u32), ObjectType::Semaphore, [&] {
        auto semaphore = std::make_shared<KernelObject>(ObjectType::Semaphore);
        semaphore->signalState = int32_t(count);
        semaphore->limit = int32_t(limit);
        AttachGuestObject(semaphore, kDispatcherSemaphore);
        mem::Store32(semaphore->guestObject + 16, limit);
        return semaphore;
    }, status);
    FinishCreate(ctx, ctx.r3.u32, object, status);
}

// NtReleaseSemaphore(handle, releaseCount, previousCount*)
GUEST_FUNC(NtReleaseSemaphore)
{
    auto object = ObjectFromHandle(ctx.r3.u32);
    if (object == nullptr)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }
    int32_t previous = ReleaseSemaphore(object, int32_t(ctx.r4.u32));
    if (previous < 0)
    {
        ctx.r3.u64 = X_STATUS_SEMAPHORE_LIMIT_EXCEEDED;
        return;
    }
    if (ctx.r5.u32 != 0)
        mem::Store32(ctx.r5.u32, uint32_t(previous));
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// KeInitializeSemaphore(semaphore, count, limit)
GUEST_FUNC(KeInitializeSemaphore)
{
    uint32_t semaphore = ctx.r3.u32;
    mem::Store8(semaphore, uint8_t(kDispatcherSemaphore));
    mem::Store8(semaphore + 2, 5);
    mem::Store32(semaphore + 4, ctx.r4.u32);
    mem::Store32(semaphore + 8, semaphore + 8);
    mem::Store32(semaphore + 12, semaphore + 8);
    mem::Store32(semaphore + 16, ctx.r5.u32);
    auto object = ObjectFromGuest(semaphore);
    object->signalState = int32_t(ctx.r4.u32);
    object->limit = int32_t(ctx.r5.u32);
}

// KeReleaseSemaphore(semaphore, increment, adjustment, wait) -> previous count
GUEST_FUNC(KeReleaseSemaphore)
{
    int32_t previous = ReleaseSemaphore(ObjectFromGuest(ctx.r3.u32), int32_t(ctx.r5.u32));
    ctx.r3.u64 = uint32_t(std::max(previous, 0));
}

// ---- Mutants ----

// NtCreateMutant(handle*, objectAttributes, initialOwner)
GUEST_FUNC(NtCreateMutant)
{
    bool initialOwner = ctx.r5.u32 != 0;
    uint32_t status;
    auto object = CreateNamedObject(ObjectName(ctx.r4.u32), ObjectType::Mutant, [&] {
        auto mutant = std::make_shared<KernelObject>(ObjectType::Mutant);
        if (initialOwner)
        {
            mutant->owner = CurrentThread();
            mutant->recursion = 1;
            mutant->signalState = 0;
        }
        else
        {
            mutant->signalState = 1;
        }
        AttachGuestObject(mutant, kDispatcherMutant);
        return mutant;
    }, status);
    FinishCreate(ctx, ctx.r3.u32, object, status);
}

// NtReleaseMutant(handle, previousCount*)
GUEST_FUNC(NtReleaseMutant)
{
    auto object = ObjectFromHandle(ctx.r3.u32);
    if (object == nullptr)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }
    int32_t previous = 0;
    if (!ReleaseMutant(object, &previous))
    {
        ctx.r3.u64 = X_STATUS_MUTANT_NOT_OWNED;
        return;
    }
    if (ctx.r4.u32 != 0)
        mem::Store32(ctx.r4.u32, uint32_t(previous));
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// ---- Timers (signal only; APC routines are not delivered yet) ----

namespace
{
    struct TimerObject : KernelObject
    {
        TimerObject() : KernelObject(ObjectType::Timer) {}
        std::atomic<uint64_t> generation{ 0 };
    };
}

// NtCreateTimer(handle*, objectAttributes, timerType)
GUEST_FUNC(NtCreateTimer)
{
    bool manualReset = ctx.r5.u32 == 0; // NotificationTimer
    uint32_t status;
    auto object = CreateNamedObject(ObjectName(ctx.r4.u32), ObjectType::Timer, [&]() -> ObjectPtr {
        auto timer = std::make_shared<TimerObject>();
        timer->manualReset = manualReset;
        AttachGuestObject(timer, kDispatcherTimer + (manualReset ? 0 : 1));
        return timer;
    }, status);
    FinishCreate(ctx, ctx.r3.u32, object, status);
}

// NtSetTimerEx(handle, dueTime*, apcRoutine, apcMode, apcContext, resume, period, previousState*)
GUEST_FUNC(NtSetTimerEx)
{
    auto object = std::dynamic_pointer_cast<TimerObject>(ObjectFromHandle(ctx.r3.u32));
    if (object == nullptr)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }
    int64_t due = LoadLargeInteger(ctx.r4.u32);
    if (due > 0)
        due = std::max<int64_t>(0, due - int64_t(SystemTime()));
    else
        due = -due;
    uint32_t period = ctx.r10.u32; // milliseconds
    if (ctx.r5.u32 != 0)
        Log("warning: NtSetTimerEx APC routine %08X is not delivered", ctx.r5.u32);

    ResetEvent(object);
    uint64_t generation = ++object->generation;
    std::thread([object, due, period, generation] {
        auto next = std::chrono::steady_clock::now() + std::chrono::nanoseconds(due * 100);
        do
        {
            std::this_thread::sleep_until(next);
            if (object->generation != generation)
                return;
            SignalObject(object);
            next += std::chrono::milliseconds(period);
        } while (period != 0);
    }).detach();
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// NtCancelTimer(handle, currentState*)
GUEST_FUNC(NtCancelTimer)
{
    auto object = std::dynamic_pointer_cast<TimerObject>(ObjectFromHandle(ctx.r3.u32));
    if (object == nullptr)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }
    object->generation++;
    if (ctx.r4.u32 != 0)
        mem::Store32(ctx.r4.u32, uint32_t(object->signalState));
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// ---- Waits ----

// NtWaitForSingleObjectEx(handle, waitMode, alertable, timeout*)
GUEST_FUNC(NtWaitForSingleObjectEx)
{
    auto object = ObjectFromHandle(ctx.r3.u32);
    if (object == nullptr)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }
    ctx.r3.u64 = Wait(&object, 1, false, ctx.r6.u32);
}

// NtWaitForMultipleObjectsEx(count, handles, waitType, waitMode, alertable, timeout*)
GUEST_FUNC(NtWaitForMultipleObjectsEx)
{
    uint32_t count = ctx.r3.u32;
    std::vector<ObjectPtr> objects(count);
    for (uint32_t i = 0; i < count; i++)
    {
        objects[i] = ObjectFromHandle(mem::Load32(ctx.r4.u32 + i * 4));
        if (objects[i] == nullptr)
        {
            ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
            return;
        }
    }
    ctx.r3.u64 = Wait(objects.data(), count, ctx.r5.u32 == 0, ctx.r8.u32);
}

// KeWaitForSingleObject(object, waitReason, waitMode, alertable, timeout*)
GUEST_FUNC(KeWaitForSingleObject)
{
    auto object = ObjectFromGuest(ctx.r3.u32);
    ctx.r3.u64 = Wait(&object, 1, false, ctx.r7.u32);
}

// KeWaitForMultipleObjects(count, objects, waitType, waitReason, waitMode, alertable, timeout*, waitBlocks)
GUEST_FUNC(KeWaitForMultipleObjects)
{
    uint32_t count = ctx.r3.u32;
    std::vector<ObjectPtr> objects(count);
    for (uint32_t i = 0; i < count; i++)
        objects[i] = ObjectFromGuest(mem::Load32(ctx.r4.u32 + i * 4));
    ctx.r3.u64 = Wait(objects.data(), count, ctx.r5.u32 == 0, ctx.r9.u32);
}

// ---- Critical sections ----
// Guest RTL_CRITICAL_SECTION: dispatcher header (used as a synchronization event for waiters),
// lock count at +0x10 (-1 when free), recursion count at +0x14, owning KTHREAD at +0x18.

namespace
{
    void InitializeCriticalSection(uint32_t cs, uint32_t spinCount)
    {
        mem::Store8(cs + 0, kSynchronizationEvent);
        mem::Store8(cs + 1, uint8_t((spinCount + 255) / 256));
        mem::Store8(cs + 2, 4);
        mem::Store32(cs + 4, 0);
        mem::Store32(cs + 8, cs + 8);
        mem::Store32(cs + 12, cs + 8);
        mem::Store32(cs + 0x10, 0xFFFFFFFF);
        mem::Store32(cs + 0x14, 0);
        mem::Store32(cs + 0x18, 0);
    }
}

GUEST_FUNC(RtlInitializeCriticalSection)
{
    InitializeCriticalSection(ctx.r3.u32, 0);
}

GUEST_FUNC(RtlInitializeCriticalSectionAndSpinCount)
{
    InitializeCriticalSection(ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

GUEST_FUNC(RtlEnterCriticalSection)
{
    uint32_t cs = ctx.r3.u32;
    uint32_t self = CurrentKThread();

    if (mem::Load32(cs + 0x18) == self)
    {
        mem::AtomicAdd32(cs + 0x10, 1);
        mem::Store32(cs + 0x14, mem::Load32(cs + 0x14) + 1);
        return;
    }

    for (uint32_t spin = mem::Load8(cs + 1) * 256u; spin > 0; spin--)
    {
        if (mem::AtomicCas32(cs + 0x10, 0xFFFFFFFF, 0))
        {
            mem::Store32(cs + 0x18, self);
            mem::Store32(cs + 0x14, 1);
            return;
        }
    }

    if (mem::AtomicAdd32(cs + 0x10, 1) != 0)
    {
        auto object = ObjectFromGuest(cs, kSynchronizationEvent);
        WaitTimeout(&object, 1, false, 0, true);
    }
    mem::Store32(cs + 0x18, self);
    mem::Store32(cs + 0x14, 1);
}

GUEST_FUNC(RtlTryEnterCriticalSection)
{
    uint32_t cs = ctx.r3.u32;
    uint32_t self = CurrentKThread();

    if (mem::AtomicCas32(cs + 0x10, 0xFFFFFFFF, 0))
    {
        mem::Store32(cs + 0x18, self);
        mem::Store32(cs + 0x14, 1);
        ctx.r3.u64 = 1;
    }
    else if (mem::Load32(cs + 0x18) == self)
    {
        mem::AtomicAdd32(cs + 0x10, 1);
        mem::Store32(cs + 0x14, mem::Load32(cs + 0x14) + 1);
        ctx.r3.u64 = 1;
    }
    else
    {
        ctx.r3.u64 = 0;
    }
}

GUEST_FUNC(RtlLeaveCriticalSection)
{
    uint32_t cs = ctx.r3.u32;
    uint32_t recursion = mem::Load32(cs + 0x14) - 1;
    mem::Store32(cs + 0x14, recursion);
    if (recursion != 0)
    {
        mem::AtomicAdd32(cs + 0x10, -1);
        return;
    }
    mem::Store32(cs + 0x18, 0);
    if (mem::AtomicAdd32(cs + 0x10, -1) != -1) // someone is waiting
        SetEvent(ObjectFromGuest(cs, kSynchronizationEvent));
}

// ---- Spinlocks and IRQL ----

namespace
{
    void AcquireSpinLock(uint32_t lock)
    {
        uint32_t self = CurrentKThread();
        while (!mem::AtomicCas32(lock, 0, self))
            std::this_thread::yield();
    }
}

GUEST_FUNC(KfAcquireSpinLock)
{
    AcquireSpinLock(ctx.r3.u32);
    ctx.r3.u64 = 0; // previous IRQL
}

GUEST_FUNC(KfReleaseSpinLock)
{
    mem::Store32(ctx.r3.u32, 0);
}

GUEST_FUNC(KeAcquireSpinLockAtRaisedIrql)
{
    AcquireSpinLock(ctx.r3.u32);
}

GUEST_FUNC(KeReleaseSpinLockFromRaisedIrql)
{
    mem::Store32(ctx.r3.u32, 0);
}

GUEST_FUNC(KeRaiseIrqlToDpcLevel) { ctx.r3.u64 = 0; }
GUEST_FUNC(KfLowerIrql) {}
GUEST_FUNC(KeEnterCriticalRegion) {}
GUEST_FUNC(KeLeaveCriticalRegion) {}
GUEST_FUNC(KeLockL2) { ctx.r3.u64 = 0; }
GUEST_FUNC(KeUnlockL2) {}
