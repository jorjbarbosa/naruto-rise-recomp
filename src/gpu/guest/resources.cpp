// Guest resource headers: decoding of surfaces and texture fetch constants, and the release of the
// host textures when the guest destroys a resource.

#include "gpu/guest/resources.h"

#include "gpu/gpu.h"
#include "gpu/guest/addresses.h"
#include "gpu/guest/buffers.h"
#include "gpu/guest/textures.h"
#include "gpu/textures/layout.h"
#include "kernel/kernel.h"

#include <mutex>
#include <unordered_set>

namespace gpu::guest
{
    namespace
    {
        // Surface format word: GPUTEXTUREFORMAT in bits 0..5.
        constexpr uint32_t kFormatMask = 0x3F;
        constexpr uint32_t kDimension2D = 1;

        void LogUncoveredFormat(uint32_t format)
        {
            static std::mutex mutex;
            static std::unordered_set<uint32_t> logged;
            std::lock_guard lock(mutex);
            if (logged.insert(format).second)
                Log("gpu: surface format %u is not covered; its clears and resolves are skipped", format);
        }
    }

    std::optional<SurfaceDesc> DescribeSurface(uint32_t header)
    {
        uint32_t size = mem::Load32(header + res::kSurfaceSize);
        uint32_t format = mem::Load32(header + res::kSurfaceFormat) & kFormatMask;
        SurfaceDesc desc;
        desc.key = header;
        desc.width = (size >> 18) + 1;
        desc.height = ((size >> 3) & 0x7FFF) + 1;
        switch (mem::Load16(header + res::kSurfaceSamples) & 3)
        {
        case 1: desc.samples = 2; break;
        case 2: desc.samples = 4; break;
        default: desc.samples = 1; break;
        }
        switch (format)
        {
        case format::k8_8_8_8:
            return desc;
        case format::k24_8:
        case format::k24_8Float:
            desc.depth = true;
            return desc;
        default:
            LogUncoveredFormat(format);
            return std::nullopt;
        }
    }

    TextureFetch ReadTextureFetch(uint32_t fetch)
    {
        textures::Fetch dwords;
        for (uint32_t i = 0; i < 6; i++)
            dwords.d[i] = mem::Load32(fetch + 4 * i);
        uint32_t border = dwords.Border() ? 2 : 0;
        TextureFetch texture;
        texture.address = GpuAddress(dwords.BaseAddress());
        texture.format = dwords.Format();
        texture.width = dwords.Width() + border;
        texture.height = dwords.Height() + border;
        texture.is2D = dwords.DimensionField() == kDimension2D;
        return texture;
    }
}

using namespace gpu::guest;

DECLARE_ORIGINAL(D3D_DESTROY_RESOURCE);

// D3D::DestroyResource(resource), reached when the last reference is released: the host side goes
// first (surfaces, textures, the copies of a vertex buffer's memory), then the original frees the header.
PPC_FUNC(D3D_DESTROY_RESOURCE)
{
    uint32_t resource = ctx.r3.u32;
    if (gpu::Renderer* renderer = gpu::ActiveRenderer())
    {
        switch (mem::Load32(resource + res::kCommon) & res::kTypeMask)
        {
        case res::kTypeSurface:
            renderer->ReleaseSurface(resource);
            break;
        case res::kTypeTexture:
            ForgetTexture(resource);
            break;
        case res::kTypeVertexBuffer:
        {
            VertexFetch fetch = DecodeVertexFetch(mem::Load32(resource + res::kVertexBufferFetch),
                mem::Load32(resource + res::kVertexBufferFetch + 4));
            ForgetBuffers(fetch.address, fetch.address + fetch.bytes);
            break;
        }
        default:
            break;
        }
    }
    ORIGINAL(D3D_DESTROY_RESOURCE)(ctx, base);
}
