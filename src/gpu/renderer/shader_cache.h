#pragma once

// The shaders of game/xeshader.sdb, translated at build time by XenosRecomp into
// build/shaders/shader_cache.cpp (scripts/recompile.ps1). That file includes this header for the
// declarations of its data.

#include <plume_render_interface_types.h>

#include <cstddef>
#include <cstdint>
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
    // The translated shaders in one format, DXIL or SPIR-V, decompressed once. Read-only after
    // construction, so any thread may use it.
    class ShaderCache
    {
    public:
        // Fatal when the format's cache does not decompress: the build is broken.
        explicit ShaderCache(plume::RenderShaderFormat format);

        // Whether the shader database has a shader with the key in this format.
        bool Has(uint64_t key) const;
        // Tells this build's shaders in this format from another build's (XXH3-64 of the compressed
        // cache).
        uint64_t Hash() const { return hash_; }
        // The code of the shader with the key, or empty when there is none or its SPIR-V does not
        // decode (logged). The SPIR-V is stored SMOL-V encoded and decoded here.
        std::vector<uint8_t> Code(uint64_t key) const;

    private:
        // The entry of the key with code in this format, or null.
        const ShaderCacheEntry* Find(uint64_t key) const;

        plume::RenderShaderFormat format_;
        std::vector<uint8_t> data_;
        uint64_t hash_ = 0;
    };
}
