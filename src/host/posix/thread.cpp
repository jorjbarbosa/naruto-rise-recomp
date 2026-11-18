// Host threads on POSIX: creation with a given stack, names and priority (thread.h).

#include "host/thread.h"

#include "host/posix/signal_stack.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>

namespace host
{
    namespace
    {
        // The longest thread name Linux keeps, without its terminator.
        constexpr size_t kThreadNameLength = 15;
        // How far below the process's nice value LowerCurrentThreadPriority puts a thread (0 normal,
        // 19 lowest).
        constexpr int kLowerNice = 5;

        // Entry of the threads of StartDetachedThread. The body moves to this stack before it runs, so
        // a thread that ExitCurrentThread ends leaves no allocation of its own behind.
        void* DetachedThreadMain(void* parameter)
        {
            posix::SignalStack signalStack;
            std::function<void()> body;
            {
                std::unique_ptr<std::function<void()>> owned(static_cast<std::function<void()>*>(parameter));
                body = std::move(*owned);
            }
            body();
            return nullptr;
        }
    }

    bool StartDetachedThread(size_t stackSize, std::function<void()> body)
    {
        auto owned = std::make_unique<std::function<void()>>(std::move(body));
        // The stack is mapped with the thread and its pages are committed as they are touched.
        pthread_attr_t attributes;
        if (pthread_attr_init(&attributes) != 0)
            return false;
        bool ok = pthread_attr_setstacksize(&attributes, stackSize) == 0 &&
            pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED) == 0;
        pthread_t thread;
        ok = ok && pthread_create(&thread, &attributes, DetachedThreadMain, owned.get()) == 0;
        pthread_attr_destroy(&attributes);
        if (ok)
            owned.release();
        return ok;
    }

    void ExitCurrentThread(uint32_t)
    {
        // Nobody reads a host thread's exit code. glibc ends the thread with a forced unwind (thread.h).
        pthread_exit(nullptr);
    }

    void SetCurrentThreadName(const char* name)
    {
        char shortName[kThreadNameLength + 1] = {};
        std::memcpy(shortName, name, std::min(std::strlen(name), kThreadNameLength));
        pthread_setname_np(pthread_self(), shortName);
    }

    void LowerCurrentThreadPriority()
    {
        // On Linux the nice value of a thread id is the thread's own, and the process id's is the
        // main thread's. A process may lower a nice value, not raise it again, which is all background
        // work needs. The level is kLowerNice below the process's, however often it is called.
        errno = 0;
        int process = getpriority(PRIO_PROCESS, id_t(getpid()));
        if (process == -1 && errno != 0)
            return;
        setpriority(PRIO_PROCESS, id_t(gettid()), std::min(process + kLowerNice, 19));
    }
}
