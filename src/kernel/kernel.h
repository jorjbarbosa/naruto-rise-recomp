#pragma once

// Shared declarations of the emulated Xbox 360 kernel (xboxkrnl.exe) and XAM (xam.xex).

#include <ppc_config.h>
#include <ppc_context.h>

#include "log.h"
#include "memory.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

// Defines the host implementation of a guest import. Overrides the weak stub from stubs.cpp.
#define GUEST_FUNC(name) PPC_FUNC(__imp__##name)

namespace kernel
{
    // NTSTATUS values.
    constexpr uint32_t X_STATUS_SUCCESS = 0x00000000;
    constexpr uint32_t X_STATUS_WAIT_0 = 0x00000000;
    constexpr uint32_t X_STATUS_ABANDONED_WAIT_0 = 0x00000080;
    constexpr uint32_t X_STATUS_USER_APC = 0x000000C0;
    constexpr uint32_t X_STATUS_ALERTED = 0x00000101;
    constexpr uint32_t X_STATUS_TIMEOUT = 0x00000102;
    constexpr uint32_t X_STATUS_PENDING = 0x00000103;
    constexpr uint32_t X_STATUS_OBJECT_NAME_EXISTS = 0x40000000;
    constexpr uint32_t X_STATUS_NO_MORE_FILES = 0x80000006;
    constexpr uint32_t X_STATUS_UNSUCCESSFUL = 0xC0000001;
    constexpr uint32_t X_STATUS_NOT_IMPLEMENTED = 0xC0000002;
    constexpr uint32_t X_STATUS_INVALID_HANDLE = 0xC0000008;
    constexpr uint32_t X_STATUS_INVALID_PARAMETER = 0xC000000D;
    constexpr uint32_t X_STATUS_NO_SUCH_FILE = 0xC000000F;
    constexpr uint32_t X_STATUS_END_OF_FILE = 0xC0000011;
    constexpr uint32_t X_STATUS_NO_MEMORY = 0xC0000017;
    constexpr uint32_t X_STATUS_ACCESS_DENIED = 0xC0000022;
    constexpr uint32_t X_STATUS_BUFFER_TOO_SMALL = 0xC0000023;
    constexpr uint32_t X_STATUS_OBJECT_TYPE_MISMATCH = 0xC0000024;
    constexpr uint32_t X_STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
    constexpr uint32_t X_STATUS_OBJECT_NAME_COLLISION = 0xC0000035;
    constexpr uint32_t X_STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;
    constexpr uint32_t X_STATUS_MUTANT_NOT_OWNED = 0xC0000046;
    constexpr uint32_t X_STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC0000047;
    constexpr uint32_t X_STATUS_INVALID_INFO_CLASS = 0xC0000003;
    constexpr uint32_t X_STATUS_INVALID_IMAGE_FORMAT = 0xC000007B;
    constexpr uint32_t X_STATUS_NOT_SUPPORTED = 0xC00000BB;
    constexpr uint32_t X_STATUS_DLL_NOT_FOUND = 0xC0000135;
    constexpr uint32_t X_STATUS_DLL_INIT_FAILED = 0xC0000142;
    constexpr uint32_t X_STATUS_DRIVER_ORDINAL_NOT_FOUND = 0xC0000262;

    // Win32 error codes returned by XAM.
    constexpr uint32_t X_ERROR_SUCCESS = 0;
    constexpr uint32_t X_ERROR_FILE_NOT_FOUND = 2;
    constexpr uint32_t X_ERROR_PATH_NOT_FOUND = 3;
    constexpr uint32_t X_ERROR_ACCESS_DENIED = 5;
    constexpr uint32_t X_ERROR_INVALID_PARAMETER = 87;
    constexpr uint32_t X_ERROR_INSUFFICIENT_BUFFER = 122;
    constexpr uint32_t X_ERROR_NO_MORE_FILES = 18;
    constexpr uint32_t X_ERROR_NOT_FOUND = 1168;
    constexpr uint32_t X_ERROR_DEVICE_NOT_CONNECTED = 1167;
    constexpr uint32_t X_ERROR_IO_PENDING = 997;
    constexpr uint32_t X_ERROR_FUNCTION_FAILED = 1627;
    constexpr uint32_t X_ERROR_NO_SUCH_USER = 1317;

    // Guest arguments past r10 live in the caller's parameter area, one 8-byte slot each.
    inline uint32_t StackArg32(const PPCContext& ctx, int index)
    {
        return mem::Load32(ctx.r1.u32 + 0x54 + (index - 8) * 8);
    }

    // Value of a guest LARGE_INTEGER pointer, or 0 when the pointer is null.
    inline int64_t LoadLargeInteger(uint32_t address, int64_t fallback = 0)
    {
        return address != 0 ? int64_t(mem::Load64(address)) : fallback;
    }

    // Reads the guest ANSI_STRING at address.
    std::string LoadAnsiString(uint32_t address);

    // Current time as a FILETIME (100 ns units since 1601).
    uint64_t SystemTime();

    // Host frequency of __rdtsc(), which the recompiled mftb uses as the time base.
    uint64_t TimeBaseFrequency();

    // ---- Kernel objects ----

    enum class ObjectType
    {
        Event,
        Semaphore,
        Mutant,
        Thread,
        Timer,
        File,
        Notification,
        Other,
    };

    struct GuestThread;

    // Dispatcher state is protected by the global dispatcher lock (objects.cpp).
    struct KernelObject
    {
        explicit KernelObject(ObjectType type) : type(type) {}
        virtual ~KernelObject() = default;

        ObjectType type;
        uint32_t guestObject = 0; // guest dispatcher object, if any
        std::string name;

        // Dispatcher state.
        bool manualReset = true; // events and timers
        int32_t signalState = 0;
        int32_t limit = 0;       // semaphores
        GuestThread* owner = nullptr; // mutants
        int32_t recursion = 0;
    };

    using ObjectPtr = std::shared_ptr<KernelObject>;

    // Handles.
    uint32_t CreateHandle(ObjectPtr object);
    ObjectPtr ObjectFromHandle(uint32_t handle);
    bool CloseHandle(uint32_t handle);
    uint32_t HandleForObject(const ObjectPtr& object);

    // Every handle-based object gets a guest dispatcher object, so ObReferenceObjectByHandle and
    // the Ke* functions (which take guest pointers) reach the same host object.
    void AttachGuestObject(const ObjectPtr& object, uint32_t dispatcherType);

    // Host object for a guest dispatcher object, created from its header on first use.
    // forcedType overrides the header (critical sections use their header as a sync event).
    ObjectPtr ObjectFromGuest(uint32_t address, int forcedDispatcherType = -1);

    // Dispatcher operations. Timeout is a guest LARGE_INTEGER pointer (0 = infinite).
    uint32_t Wait(const ObjectPtr* objects, uint32_t count, bool waitAll, uint32_t timeoutAddress);
    uint32_t WaitTimeout(const ObjectPtr* objects, uint32_t count, bool waitAll, int64_t timeout100ns, bool infinite);
    int32_t SetEvent(const ObjectPtr& object);
    int32_t ResetEvent(const ObjectPtr& object);
    int32_t PulseEvent(const ObjectPtr& object);
    // Returns the previous count, or -1 when the limit would be exceeded.
    int32_t ReleaseSemaphore(const ObjectPtr& object, int32_t count);
    // Returns false when the current thread does not own the mutant.
    bool ReleaseMutant(const ObjectPtr& object, int32_t* previous);
    void SignalObject(const ObjectPtr& object); // threads (exit) and timers

    ObjectPtr CreateEvent(bool manualReset, bool initialState);

    // NtCreate* with a name: when the name (case-insensitive) is already in use, returns the existing
    // object and sets status to X_STATUS_OBJECT_NAME_EXISTS, or returns nullptr with
    // X_STATUS_OBJECT_TYPE_MISMATCH when that object has another type. Otherwise calls create(),
    // registers the result under the name and sets status to X_STATUS_SUCCESS. An empty name just
    // calls create(). The name is released when the last handle to the object closes.
    ObjectPtr CreateNamedObject(const std::string& name, ObjectType type,
        const std::function<ObjectPtr()>& create, uint32_t& status);

    // ---- Threads ----

    struct GuestThread
    {
        uint32_t id = 0;
        uint32_t kthread = 0;   // guest KTHREAD, also the guest object of the thread
        uint32_t pcr = 0;       // guest KPCR, loaded into r13
        uint32_t stackBase = 0; // lowest address
        uint32_t stackSize = 0;
        std::string name;
        std::shared_ptr<KernelObject> object;
        int32_t suspendCount = 0;
        uint32_t tls[64] = {};
    };

    GuestThread* CurrentThread();
    GuestThread* ThreadById(uint32_t id);
    GuestThread* ThreadByObject(uint32_t kthread);
    std::shared_ptr<KernelObject> ThreadObject(GuestThread* thread);

    // Runs the main thread: sets up stack and KPCR, then calls entry (blocking).
    void RunMainThread(PPCFunc* entry, uint32_t stackSize);

    // Starts a guest thread that runs host code, for the runtime's own drivers. Like a title thread
    // it has a KTHREAD, a KPCR and a guest stack, and it starts on the processor of the calling
    // guest thread. body runs on a new host thread with r1 and r13 set up, so it can call guest
    // functions with ctx; the guest thread ends when body returns. Returns the new thread.
    GuestThread* StartSystemThread(const char* name, std::function<void(PPCContext&)> body);

    // Resumes a suspended thread; returns the previous suspend count.
    int32_t ResumeThread(GuestThread* thread);

    // ---- Startup ----

    struct ModuleInfo
    {
        uint32_t imageBase = 0;
        uint32_t imageSize = 0;
        uint32_t entryPoint = 0;
        uint32_t xexHeader = 0;   // guest copy of the XEX header
        uint32_t moduleHandle = 0; // guest LDR_DATA_TABLE_ENTRY-like block
        uint32_t stackSize = 0;
    };
    extern ModuleInfo g_module;

    // Guest address of a kernel data export (XboxHardwareInfo, KeTimeStampBundle, ...), or 0. Every
    // module importing an export gets the same object. Thread-safe.
    uint32_t ResolveVariable(const char* module, const char* name);

    // Starts the timer that keeps KeTimeStampBundle current.
    void StartKernelClock();

    // Game folder on the host and the folder for data the game writes.
    void SetPaths(const std::string& gameDir, const std::string& userDir);

    // Host path of a guest path ("D:\AI2C.DLL", "game:\...", symbolic links the game created).
    std::filesystem::path HostPath(const std::string& guestPath);

    // Called by the stubs of imports that are not implemented.
    void ReportUnimplemented(const char* name, PPCContext& ctx);
    void PrintImportSummary();
}
