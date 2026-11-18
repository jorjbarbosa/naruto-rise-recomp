// The translated shaders: decompression of the generated DXIL or SPIR-V cache and lookup by key.

#include "gpu/renderer/shader_cache.h"

#include "log.h"

#include <smolv.h>
#include <zstd.h>

#include <algorithm>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace gpu
{
    namespace
    {
        // The generated cache of a format, compressed.
        struct CompressedCache
        {
            const uint8_t* data;
            size_t size;
            size_t decompressedSize;
        };

        CompressedCache CacheOf(plume::RenderShaderFormat format)
        {
#ifdef ROAN_D3D12
            if (format == plume::RenderShaderFormat::DXIL)
                return { g_compressedDxilCache, g_dxilCacheCompressedSize, g_dxilCacheDecompressedSize };
#else
            if (format == plume::RenderShaderFormat::DXIL)
                Fatal("gpu: this build has no DXIL shaders");
#endif
            return { g_compressedSpirvCache, g_spirvCacheCompressedSize, g_spirvCacheDecompressedSize };
        }
    }

    ShaderCache::ShaderCache(plume::RenderShaderFormat format) : format_(format)
    {
        CompressedCache cache = CacheOf(format);
        data_.resize(cache.decompressedSize);
        size_t size = ZSTD_decompress(data_.data(), data_.size(), cache.data, cache.size);
        if (ZSTD_isError(size) || size != data_.size())
            Fatal("gpu: the shader cache does not decompress (%s)", ZSTD_isError(size) ? ZSTD_getErrorName(size) : "size");
        hash_ = XXH3_64bits(cache.data, cache.size);
        Log("gpu: shader cache with %zu shaders, %zu MB of %s", g_shaderCacheEntryCount, data_.size() >> 20,
            format == plume::RenderShaderFormat::DXIL ? "DXIL" : "SMOL-V");
    }

    const ShaderCacheEntry* ShaderCache::Find(uint64_t key) const
    {
        const ShaderCacheEntry* begin = g_shaderCacheEntries;
        const ShaderCacheEntry* end = begin + g_shaderCacheEntryCount;
        const ShaderCacheEntry* entry = std::lower_bound(begin, end, key,
            [](const ShaderCacheEntry& e, uint64_t hash) { return e.hash < hash; });
        if (entry == end || entry->hash != key)
            return nullptr;
        bool dxil = format_ == plume::RenderShaderFormat::DXIL;
        uint64_t offset = dxil ? entry->dxilOffset : entry->spirvOffset;
        uint64_t size = dxil ? entry->dxilSize : entry->spirvSize;
        if (size == 0 || offset + size > data_.size())
            return nullptr;
        return entry;
    }

    bool ShaderCache::Has(uint64_t key) const
    {
        return Find(key) != nullptr;
    }

    std::vector<uint8_t> ShaderCache::Code(uint64_t key) const
    {
        const ShaderCacheEntry* entry = Find(key);
        if (entry == nullptr)
            return {};
        if (format_ == plume::RenderShaderFormat::DXIL)
            return { data_.data() + entry->dxilOffset, data_.data() + entry->dxilOffset + entry->dxilSize };

        const uint8_t* smolv = data_.data() + entry->spirvOffset;
        std::vector<uint8_t> spirv(smolv::GetDecodedBufferSize(smolv, entry->spirvSize));
        if (spirv.empty() || !smolv::Decode(smolv, entry->spirvSize, spirv.data(), spirv.size()))
        {
            Log("gpu: the SPIR-V of shader %016llX does not decode", (unsigned long long)key);
            return {};
        }
        return spirv;
    }
}
