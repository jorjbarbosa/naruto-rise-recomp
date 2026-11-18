// Host threads on Windows: creation with a given stack, names and priority (thread.h).

#include "host/thread.h"

#include <cstring>
#include <memory>
#include <string>
#include <windows.h>

namespace host
{
    namespace
    {
        // Entry of the threads of StartDetachedThread. The body moves to this stack before it runs, so
        // a thread that ExitCurrentThread ends leaves no allocation of its own behind.
        DWORD WINAPI DetachedThreadMain(void* parameter)
        {
            std::function<void()> body;
            {
                std::unique_ptr<std::function<void()>> owned(static_cast<std::function<void()>*>(parameter));
                body = std::move(*owned);
            }
            body();
            return 0;
        }
    }

    bool StartDetachedThread(size_t stackSize, std::function<void()> body)
    {
        auto owned = std::make_unique<std::function<void()>>(std::move(body));
        HANDLE thread = CreateThread(nullptr, stackSize, DetachedThreadMain, owned.get(), STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (thread == nullptr)
            return false;
        owned.release();
        CloseHandle(thread);
        return true;
    }

    void ExitCurrentThread(uint32_t exitCode)
    {
        ExitThread(exitCode);
    }

    void SetCurrentThreadName(const char* name)
    {
        std::wstring wide(name, name + std::strlen(name));
        SetThreadDescription(GetCurrentThread(), wide.c_str());
    }

    void LowerCurrentThreadPriority()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    }
}
