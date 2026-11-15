// Game patches: replacements for recompiled functions of default.xex. Recompiled functions are
// weak aliases (sub_XXXXXXXX) of their implementation (__imp__sub_XXXXXXXX), so a definition here
// replaces every call to them.

#include "kernel/kernel.h"

#include <thread>

extern "C" PPC_FUNC(__imp__sub_8265BB68);

namespace
{
    // cLoader object: the function its thread runs, and that function's argument.
    constexpr uint32_t kLoaderFunction = 236;
}

// XFileCacheInit (XAPI): the file cache copies DVD files to the hard disk cache partition and then
// serves reads from there. It has no use on PC and there is no cache partition, so report a
// failure: the game then skips the whole cache setup (sub_82686060) and reads its files directly
// from game:\.
PPC_FUNC(sub_82176D30)
{
    TRACE("XFileCacheInit refused");
    ctx.r3.u64 = kernel::X_ERROR_FUNCTION_FAILED;
}

// cLoader::Run (vtable entry 6 of the loader's thread class): names the thread, then calls the
// function at +236 with the argument at +240. The constructor (sub_8265BDE0) starts the thread
// (vtable entry 7) and only then stores those two words. On the console the new thread does not run
// before its creator gets there; here it can, and it called address 0. Replaced to wait for the
// function before running the original; the argument is stored right after it, in program order.
PPC_FUNC(sub_8265BB68)
{
    while (mem::AtomicLoad32(ctx.r3.u32 + kLoaderFunction) == 0)
        std::this_thread::yield();
    __imp__sub_8265BB68(ctx, base);
}
