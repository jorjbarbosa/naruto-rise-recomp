#pragma once

// Included by the generated ppc_config.h (PPC_INCLUDE_DETAIL). Hooks the runtime needs inside the
// recompiled code.

#include <cstdint>

struct PPCContext;

// Called when the guest jumps through a pointer that is not a known function.
[[noreturn]] void ReportBadIndirectCall(uint32_t target, PPCContext& ctx);

// Calls a function outside the calling module's code: default.xex calling into AI2C.dll (its exports,
// the compiled AI scripts) and back (the AI natives). Reports a bad call when no loaded module has
// the target.
void CallForeignFunction(uint32_t target, PPCContext& ctx, uint8_t* base);

// Same lookup as the default one, but with a range check, calls across modules and a readable error
// instead of a jump to garbage. PPC_LOOKUP_FUNC and PPCFunc come from ppc_context.h, which is
// included later; PPC_CODE_BASE and PPC_CODE_SIZE are those of the module being compiled.
#define PPC_CALL_INDIRECT_FUNC(x)                                                         \
    do                                                                                    \
    {                                                                                     \
        const uint32_t indirectTarget_ = (x);                                             \
        if (indirectTarget_ - uint32_t(PPC_CODE_BASE) >= uint32_t(PPC_CODE_SIZE))         \
        {                                                                                 \
            CallForeignFunction(indirectTarget_, ctx, base);                              \
            break;                                                                        \
        }                                                                                 \
        PPCFunc* indirectFunc_ = PPC_LOOKUP_FUNC(base, indirectTarget_);                  \
        if (indirectFunc_ == nullptr)                                                     \
            ReportBadIndirectCall(indirectTarget_, ctx);                                  \
        indirectFunc_(ctx, base);                                                         \
    } while (0)
