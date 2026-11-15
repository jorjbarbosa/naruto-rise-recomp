// D3D queries: occlusion query results.

#include "gpu/guest/addresses.h"
#include "kernel/kernel.h"

namespace gpu::guest
{
    namespace
    {
        // The samples an occlusion query reports: some, so that the object counts as visible.
        constexpr uint32_t kVisibleSamples = 1000;
    }
}

using namespace gpu::guest;

DECLARE_ORIGINAL(D3D_QUERY_GET_DATA);

// D3DQuery_GetData(query, data, size, flags): 0 when the result is in data, 1 when not yet. An
// occlusion query's count comes from ZPASS_DONE events, whose packets land in the discard segment,
// and the original reads the count block through the cached physical view (GPU address +
// 0xC0000000), which this runtime does not map onto the physical heap: it would report every query
// done with 0 samples, so the object would always count as hidden. Replaced to report a ready
// occlusion query as visible instead; the original still decides readiness (fence) and kicks.
PPC_FUNC(D3D_QUERY_GET_DATA)
{
    uint32_t query = ctx.r3.u32;
    uint32_t data = ctx.r4.u32;
    ORIGINAL(D3D_QUERY_GET_DATA)(ctx, base);
    if (ctx.r3.u32 != 0 || data == 0 || mem::Load32(query + query::kType) != query::kTypeOcclusion)
        return;
    LOG_ONCE("gpu: occlusion queries are not counted; each reports %u samples (visible)", kVisibleSamples);
    mem::Store32(data, kVisibleSamples);
}
