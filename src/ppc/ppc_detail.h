#pragma once

// Included by the generated ppc_config.h (PPC_INCLUDE_DETAIL). Hooks the runtime needs inside the
// recompiled code.

#include <cstdint>

struct PPCContext;

// Called when the guest jumps through a pointer that is not a known function.
[[noreturn]] void ReportBadIndirectCall(uint32_t target, PPCContext& ctx);

// Same lookup as the default one, but with a range check and a readable error instead of a jump to
// garbage. PPC_LOOKUP_FUNC and PPCFunc come from ppc_context.h, which is included later.
#define PPC_CALL_INDIRECT_FUNC(x)                                                         \
    do                                                                                    \
    {                                                                                     \
        const uint32_t indirectTarget_ = (x);                                             \
        if (indirectTarget_ - uint32_t(PPC_CODE_BASE) >= uint32_t(PPC_CODE_SIZE))         \
            ReportBadIndirectCall(indirectTarget_, ctx);                                  \
        PPCFunc* indirectFunc_ = PPC_LOOKUP_FUNC(base, indirectTarget_);                  \
        if (indirectFunc_ == nullptr)                                                     \
            ReportBadIndirectCall(indirectTarget_, ctx);                                  \
        indirectFunc_(ctx, base);                                                         \
    } while (0)
