// The translated shaders: decompression of the generated DXIL cache and lookup by key.

#include "gpu/renderer/shader_cache.h"

#include "log.h"

#include <zstd.h>

#include <algorithm>

namespace gpu
{
    ShaderCache::ShaderCache()
    {
        dxil_.resize(g_dxilCacheDecompressedSize);
        size_t size = ZSTD_decompress(dxil_.data(), dxil_.size(), g_compressedDxilCache, g_dxilCacheCompressedSize);
        if (ZSTD_isError(size) || size != dxil_.size())
            Fatal("gpu: the shader cache does not decompress (%s)", ZSTD_isError(size) ? ZSTD_getErrorName(size) : "size");
        Log("gpu: shader cache with %zu shaders, %zu MB of DXIL", g_shaderCacheEntryCount, dxil_.size() >> 20);
    }

    std::span<const uint8_t> ShaderCache::Dxil(uint64_t key) const
    {
        const ShaderCacheEntry* begin = g_shaderCacheEntries;
        const ShaderCacheEntry* end = begin + g_shaderCacheEntryCount;
        const ShaderCacheEntry* entry = std::lower_bound(begin, end, key,
            [](const ShaderCacheEntry& e, uint64_t hash) { return e.hash < hash; });
        if (entry == end || entry->hash != key || uint64_t(entry->dxilOffset) + entry->dxilSize > dxil_.size())
            return {};
        return { dxil_.data() + entry->dxilOffset, entry->dxilSize };
    }
}
