// Guest resource headers: decoding of surfaces and texture fetch constants, and the release of the
// host textures when the guest destroys a resource.

#include "gpu/guest/resources.h"

#include "gpu/gpu.h"
#include "gpu/guest/addresses.h"
#include "kernel/kernel.h"

#include <mutex>
#include <unordered_set>

namespace gpu::guest
{
    namespace
    {
        // Texture fetch constant fields (Xenos xe_gpu_texture_fetch_t), by dword.
        constexpr uint32_t kFetchBaseMask = 0xFFFFF000; // dword 1
        constexpr uint32_t kFetchFormatMask = 0x3F;     // dword 1
        constexpr uint32_t kFetchSizeMask = 0x1FFF;     // dword 2: width - 1, height - 1 above it
        constexpr uint32_t kFetchSizeShift = 13;
        constexpr uint32_t kFetchBorder = 0x80000000;   // dword 3: a one-texel border on every side
        constexpr uint32_t kFetchDimensionShift = 9;    // dword 5, 2 bits
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
        uint32_t format = mem::Load32(header + res::kSurfaceFormat) & kFetchFormatMask;
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
        uint32_t base = mem::Load32(fetch + 4);
        uint32_t size = mem::Load32(fetch + 8);
        uint32_t border = (mem::Load32(fetch + 12) & kFetchBorder) != 0 ? 2 : 0;
        TextureFetch texture;
        texture.address = GpuAddress(base & kFetchBaseMask);
        texture.format = base & kFetchFormatMask;
        texture.width = (size & kFetchSizeMask) + 1 + border;
        texture.height = ((size >> kFetchSizeShift) & kFetchSizeMask) + 1 + border;
        texture.is2D = ((mem::Load32(fetch + 20) >> kFetchDimensionShift) & 3) == kDimension2D;
        return texture;
    }
}

using namespace gpu::guest;

DECLARE_ORIGINAL(D3D_DESTROY_RESOURCE);

// D3D::DestroyResource(resource), reached when the last reference is released: the host side goes
// first, then the original frees the header.
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
            renderer->ReleaseTexture(ReadTextureFetch(resource + res::kTextureFetch).address, resource);
            break;
        default:
            break;
        }
    }
    ORIGINAL(D3D_DESTROY_RESOURCE)(ctx, base);
}
