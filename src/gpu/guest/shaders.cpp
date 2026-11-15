// The registry of the D3D shader objects, filled by hooks on the Jade shader database.

#include "gpu/guest/shaders.h"

#include "gpu/guest/addresses.h"
#include "kernel/kernel.h"

#include <mutex>
#include <unordered_map>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace gpu::guest
{
    namespace
    {
        // Larger containers do not occur in the shader database; a size past these is not a
        // shader object.
        constexpr uint32_t kMaxVirtualSize = 0x10000;
        constexpr uint32_t kMaxPhysicalSize = 0x40000;
        constexpr uint32_t kMaxElements = 16;

        std::mutex g_mutex;
        std::unordered_map<uint32_t, std::shared_ptr<const ShaderInfo>> g_shaders;

        // The shader of an object just created, or null when its container does not read as one
        // (logged once).
        std::shared_ptr<ShaderInfo> ReadShader(uint32_t object, bool pixel)
        {
            uint32_t container = object + (pixel ? shader::kPixelContainer : shader::kVertexContainer);
            uint32_t physical = mem::Load32(object + (pixel ? shader::kPixelPhysical : shader::kVertexPhysical));
            uint32_t virtualSize = mem::Load32(container + shader::kVirtualSize);
            uint32_t physicalSize = mem::Load32(container + shader::kPhysicalSize);
            uint32_t header = container + mem::Load32(container + shader::kShaderOffset);
            if (virtualSize > kMaxVirtualSize || physicalSize > kMaxPhysicalSize || physical == 0 ||
                header + shader::kCodeSize + 4 > container + virtualSize)
            {
                LOG_ONCE("gpu: shader object %08X has an unexpected container (virtual %u, physical %u bytes)", object,
                    virtualSize, physicalSize);
                return nullptr;
            }

            // The key of the shader cache: the container as the shader database stores it, the
            // virtual part followed by the physical part.
            XXH3_state_t state;
            XXH3_64bits_reset(&state);
            XXH3_64bits_update(&state, mem::Ptr(container), virtualSize);
            XXH3_64bits_update(&state, mem::Ptr(physical), physicalSize);

            auto info = std::make_shared<ShaderInfo>();
            info->key = XXH3_64bits_digest(&state);
            info->pixel = pixel;
            if (pixel)
                return info;

            uint32_t codeOffset = mem::Load32(header + shader::kCodeOffset);
            uint32_t codeSize = mem::Load32(header + shader::kCodeSize);
            uint32_t base = mem::Load32(header + shader::kElementBase);
            uint32_t count = mem::Load32(header + shader::kElementCount);
            if (uint64_t(codeOffset) + codeSize > physicalSize || count > kMaxElements ||
                header + shader::kElements + 4 * (uint64_t(base) + count) > container + virtualSize)
            {
                LOG_ONCE("gpu: vertex shader object %08X has an unexpected header", object);
                return nullptr;
            }
            info->code.resize(codeSize / 4);
            for (uint32_t i = 0; i < codeSize / 4; i++)
                info->code[i] = mem::Load32(physical + codeOffset + 4 * i);
            for (uint32_t i = 0; i < count; i++)
            {
                uint32_t value = mem::Load32(header + shader::kElements + 4 * (base + i));
                info->elements.push_back({ value & 0xFFF, uint8_t((value >> 12) & 0xF), uint8_t((value >> 16) & 0xF) });
            }
            return info;
        }

        void Register(uint32_t object, bool pixel)
        {
            if (object == 0)
                return;
            std::shared_ptr<ShaderInfo> info = ReadShader(object, pixel);
            std::lock_guard lock(g_mutex);
            if (info)
                g_shaders[object] = std::move(info);
            else
                g_shaders.erase(object);
        }
    }

    std::shared_ptr<const ShaderInfo> FindShader(uint32_t object)
    {
        std::lock_guard lock(g_mutex);
        auto it = g_shaders.find(object);
        return it != g_shaders.end() ? it->second : nullptr;
    }
}

DECLARE_ORIGINAL(JADE_CREATE_VERTEX_SHADER);
DECLARE_ORIGINAL(JADE_CREATE_PIXEL_SHADER);

// (wrapper, shader database entry, constant table): builds a vertex shader object from the entry
// and stores it at wrapper + 4. The object is registered right away, before any draw lets the D3D
// library patch its vertex fetches.
PPC_FUNC(JADE_CREATE_VERTEX_SHADER)
{
    uint32_t wrapper = ctx.r3.u32;
    ORIGINAL(JADE_CREATE_VERTEX_SHADER)(ctx, base);
    if (wrapper != 0)
        gpu::guest::Register(mem::Load32(wrapper + 4), false);
}

// The same for a pixel shader object.
PPC_FUNC(JADE_CREATE_PIXEL_SHADER)
{
    uint32_t wrapper = ctx.r3.u32;
    ORIGINAL(JADE_CREATE_PIXEL_SHADER)(ctx, base);
    if (wrapper != 0)
        gpu::guest::Register(mem::Load32(wrapper + 4), true);
}
