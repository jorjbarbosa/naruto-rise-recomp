// Weak stubs for every import of the game (list generated from build/ppc by CMake). The real
// implementations in the other files override them. A stub logs its first call and returns 0.

#include "kernel/kernel.h"

#include <map>
#include <mutex>
#include <string>

namespace kernel
{
    static std::mutex g_unimplementedMutex;
    static std::map<std::string, uint64_t> g_unimplemented;

    void ReportUnimplemented(const char* name, PPCContext& ctx)
    {
        {
            std::lock_guard lock(g_unimplementedMutex);
            if (g_unimplemented[name]++ == 0)
            {
                Log("unimplemented: %s (lr=%08X r3=%08X r4=%08X r5=%08X r6=%08X)", name, uint32_t(ctx.lr),
                    ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
            }
        }
        ctx.r3.u64 = 0;
    }

    void PrintImportSummary()
    {
        std::lock_guard lock(g_unimplementedMutex);
        if (g_unimplemented.empty())
            return;
        Log("unimplemented imports called:");
        for (auto& [name, count] : g_unimplemented)
            Log("  %8llu  %s", (unsigned long long)count, name.c_str());
    }
}

#define XBOX_IMPORT(name)                                   \
    PPC_WEAK_FUNC(__imp__##name)                            \
    {                                                       \
        kernel::ReportUnimplemented(#name, ctx);            \
    }
#include "imports.inc"
