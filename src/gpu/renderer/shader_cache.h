#pragma once

// The shaders of game/xeshader.sdb, translated at build time by XenosRecomp into
// build/shaders/shader_cache.cpp (scripts/recompile.ps1). That file includes this header for the
// declarations of its data.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// An entry of the generated table, which is sorted by hash. The hash (XXH3-64 of the shader
// container, virtual part then physical part) is the key the draws use.
struct ShaderCacheEntry
{
    uint64_t hash;
    uint32_t dxilOffset;
    uint32_t dxilSize;
    uint32_t spirvOffset;
    uint32_t spirvSize;
    uint32_t specConstantsMask;
};

extern ShaderCacheEntry g_shaderCacheEntries[];
extern const size_t g_shaderCacheEntryCount;
extern const uint8_t g_compressedDxilCache[];
extern const size_t g_dxilCacheCompressedSize;
extern const size_t g_dxilCacheDecompressedSize;
extern const uint8_t g_compressedSpirvCache[];
extern const size_t g_spirvCacheCompressedSize;
extern const size_t g_spirvCacheDecompressedSize;

namespace gpu
{
    // The DXIL of the translated shaders, decompressed once. Read-only after construction.
    class ShaderCache
    {
    public:
        // Fatal when the cache does not decompress: the build is broken.
        ShaderCache();

        // The DXIL of the shader with the key, or empty when the shader database has no such shader.
        std::span<const uint8_t> Dxil(uint64_t key) const;

    private:
        std::vector<uint8_t> dxil_;
    };
}
