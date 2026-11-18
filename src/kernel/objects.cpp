// Handles, guest dispatcher objects and waits.

#include "kernel/kernel.h"

#include "ascii.h"
#include "host/timing.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace kernel
{
    // Guest DISPATCHER_HEADER: type (u8), absolute (u8), size (u8), inserted (u8),
    // signal state (s32), wait list (8 bytes).
    enum DispatcherType : uint32_t
    {
        kNotificationEvent = 0,
        kSynchronizationEvent = 1,
        kMutant = 2,
        kSemaphore = 5,
        kThread = 6,
        kNotificationTimer = 8,
        kSynchronizationTimer = 9,
    };

    static std::mutex g_handleMutex;
    static std::unordered_map<uint32_t, ObjectPtr> g_handles;
    static uint32_t g_nextHandle = 0xF8000004;

    static std::mutex g_guestMutex;
    static std::unordered_map<uint32_t, ObjectPtr> g_guestObjects;

    // Lower-case name -> object. Weak, so a name goes away with the last handle to its object.
    static std::mutex g_namedMutex;
    static std::unordered_map<std::string, std::weak_ptr<KernelObject>> g_namedObjects;

    // One lock and one condition variable for all dispatcher state: simple and correct.
    static std::mutex g_dispatchMutex;
    static std::condition_variable g_dispatchCv;

    uint32_t CreateHandle(ObjectPtr object)
    {
        std::lock_guard lock(g_handleMutex);
        uint32_t handle = g_nextHandle;
        g_nextHandle += 4;
        g_handles[handle] = std::move(object);
        return handle;
    }

    ObjectPtr ObjectFromHandle(uint32_t handle)
    {
        if (handle == 0xFFFFFFFE) // NtCurrentThread()
            return ThreadObject(CurrentThread());

        std::lock_guard lock(g_handleMutex);
        auto it = g_handles.find(handle);
        return it != g_handles.end() ? it->second : nullptr;
    }

    bool CloseHandle(uint32_t handle)
    {
        std::lock_guard lock(g_handleMutex);
        return g_handles.erase(handle) != 0;
    }

    uint32_t HandleForObject(const ObjectPtr& object)
    {
        {
            std::lock_guard lock(g_handleMutex);
            for (auto& [handle, value] : g_handles)
            {
                if (value == object)
                    return handle;
            }
        }
        return CreateHandle(object);
    }

    static void StoreSignalState(const ObjectPtr& object)
    {
        if (object->guestObject != 0)
            mem::Store32(object->guestObject + 4, uint32_t(object->signalState));
    }

    void AttachGuestObject(const ObjectPtr& object, uint32_t dispatcherType)
    {
        uint32_t size = dispatcherType == kThread ? 0xB00 : 0x40;
        uint32_t address = mem::Alloc(mem::Heap::System, size);
        mem::Store8(address, uint8_t(dispatcherType));
        mem::Store8(address + 2, uint8_t(size / 4));
        mem::Store32(address + 4, uint32_t(object->signalState));
        mem::Store32(address + 8, address + 8); // empty wait list
        mem::Store32(address + 12, address + 8);
        object->guestObject = address;

        std::lock_guard lock(g_guestMutex);
        g_guestObjects[address] = object;
    }

    ObjectPtr ObjectFromGuest(uint32_t address, int forcedDispatcherType)
    {
        std::lock_guard lock(g_guestMutex);
        auto it = g_guestObjects.find(address);
        if (it != g_guestObjects.end())
            return it->second;

        uint32_t type = forcedDispatcherType >= 0 ? uint32_t(forcedDispatcherType) : mem::Load8(address);
        ObjectPtr object;
        switch (type)
        {
        case kNotificationEvent:
        case kSynchronizationEvent:
            object = std::make_shared<KernelObject>(ObjectType::Event);
            object->manualReset = type == kNotificationEvent;
            break;
        case kMutant:
            object = std::make_shared<KernelObject>(ObjectType::Mutant);
            break;
        case kSemaphore:
            object = std::make_shared<KernelObject>(ObjectType::Semaphore);
            object->limit = int32_t(mem::Load32(address + 16));
            break;
        case kNotificationTimer:
        case kSynchronizationTimer:
            object = std::make_shared<KernelObject>(ObjectType::Timer);
            object->manualReset = type == kNotificationTimer;
            break;
        default:
            Log("warning: guest dispatcher object at %08X has unknown type %u", address, type);
            object = std::make_shared<KernelObject>(ObjectType::Event);
            break;
        }
        object->signalState = int32_t(mem::Load32(address + 4));
        if (type == kMutant)
            object->signalState = 1; // free
        object->guestObject = address;
        g_guestObjects[address] = object;
        return object;
    }

    ObjectPtr CreateEvent(bool manualReset, bool initialState)
    {
        auto object = std::make_shared<KernelObject>(ObjectType::Event);
        object->manualReset = manualReset;
        object->signalState = initialState ? 1 : 0;
        AttachGuestObject(object, manualReset ? kNotificationEvent : kSynchronizationEvent);
        return object;
    }

    ObjectPtr CreateNamedObject(const std::string& name, ObjectType type,
        const std::function<ObjectPtr()>& create, uint32_t& status)
    {
        status = X_STATUS_SUCCESS;
        if (name.empty())
            return create();

        std::string key = name;
        std::transform(key.begin(), key.end(), key.begin(), ascii::ToLower);

        // Held across lookup and creation, so two threads creating the same name share one object.
        std::lock_guard lock(g_namedMutex);
        auto it = g_namedObjects.find(key);
        if (it != g_namedObjects.end())
        {
            if (ObjectPtr existing = it->second.lock())
            {
                if (existing->type != type)
                {
                    status = X_STATUS_OBJECT_TYPE_MISMATCH;
                    return nullptr;
                }
                TRACE("object: \"%s\" already exists, sharing it", name.c_str());
                status = X_STATUS_OBJECT_NAME_EXISTS;
                return existing;
            }
        }

        ObjectPtr object = create();
        object->name = name;
        g_namedObjects[key] = object;
        TRACE("object: created \"%s\"", name.c_str());
        return object;
    }

    // ---- Waits ----

    static bool IsSignaled(const KernelObject& object, GuestThread* thread)
    {
        if (object.type == ObjectType::Mutant)
            return object.signalState > 0 || object.owner == thread;
        return object.signalState > 0;
    }

    static void Acquire(KernelObject& object, GuestThread* thread)
    {
        switch (object.type)
        {
        case ObjectType::Event:
        case ObjectType::Timer:
            if (!object.manualReset)
                object.signalState = 0;
            break;
        case ObjectType::Semaphore:
            object.signalState--;
            break;
        case ObjectType::Mutant:
            object.owner = thread;
            object.recursion++;
            object.signalState = 0;
            break;
        default:
            break;
        }
    }

    uint32_t WaitTimeout(const ObjectPtr* objects, uint32_t count, bool waitAll, int64_t timeout100ns, bool infinite)
    {
        GuestThread* thread = CurrentThread();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeout100ns * 100);

        std::unique_lock lock(g_dispatchMutex);
        while (true)
        {
            if (waitAll)
            {
                bool all = true;
                for (uint32_t i = 0; i < count; i++)
                    all = all && IsSignaled(*objects[i], thread);
                if (all)
                {
                    for (uint32_t i = 0; i < count; i++)
                    {
                        Acquire(*objects[i], thread);
                        StoreSignalState(objects[i]);
                    }
                    return X_STATUS_WAIT_0;
                }
            }
            else
            {
                for (uint32_t i = 0; i < count; i++)
                {
                    if (IsSignaled(*objects[i], thread))
                    {
                        Acquire(*objects[i], thread);
                        StoreSignalState(objects[i]);
                        return X_STATUS_WAIT_0 + i;
                    }
                }
            }

            if (infinite)
                g_dispatchCv.wait(lock);
            else if (g_dispatchCv.wait_until(lock, deadline) == std::cv_status::timeout &&
                     std::chrono::steady_clock::now() >= deadline)
                return X_STATUS_TIMEOUT;
        }
    }

    uint32_t Wait(const ObjectPtr* objects, uint32_t count, bool waitAll, uint32_t timeoutAddress)
    {
        if (timeoutAddress == 0)
            return WaitTimeout(objects, count, waitAll, 0, true);

        int64_t timeout = LoadLargeInteger(timeoutAddress);
        if (timeout > 0) // absolute system time
            timeout = std::max<int64_t>(0, timeout - int64_t(host::SystemTime()));
        else
            timeout = -timeout;
        return WaitTimeout(objects, count, waitAll, timeout, false);
    }

    int32_t SetEvent(const ObjectPtr& object)
    {
        std::lock_guard lock(g_dispatchMutex);
        int32_t previous = object->signalState;
        object->signalState = 1;
        StoreSignalState(object);
        g_dispatchCv.notify_all();
        return previous;
    }

    int32_t ResetEvent(const ObjectPtr& object)
    {
        std::lock_guard lock(g_dispatchMutex);
        int32_t previous = object->signalState;
        object->signalState = 0;
        StoreSignalState(object);
        return previous;
    }

    int32_t PulseEvent(const ObjectPtr& object)
    {
        // Approximation: waiters that are already blocked get a chance to see the signal.
        int32_t previous = SetEvent(object);
        std::this_thread::yield();
        ResetEvent(object);
        return previous;
    }

    int32_t ReleaseSemaphore(const ObjectPtr& object, int32_t count)
    {
        std::lock_guard lock(g_dispatchMutex);
        int32_t previous = object->signalState;
        if (object->limit > 0 && previous + count > object->limit)
            return -1;
        object->signalState += count;
        StoreSignalState(object);
        g_dispatchCv.notify_all();
        return previous;
    }

    bool ReleaseMutant(const ObjectPtr& object, int32_t* previous)
    {
        std::lock_guard lock(g_dispatchMutex);
        if (object->owner != CurrentThread())
            return false;
        if (previous != nullptr)
            *previous = object->recursion;
        if (--object->recursion == 0)
        {
            object->owner = nullptr;
            object->signalState = 1;
            g_dispatchCv.notify_all();
        }
        return true;
    }

    void SignalObject(const ObjectPtr& object)
    {
        std::lock_guard lock(g_dispatchMutex);
        object->signalState = 1;
        StoreSignalState(object);
        g_dispatchCv.notify_all();
    }
}
