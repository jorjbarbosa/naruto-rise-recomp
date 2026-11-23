// A host cache in front of the shader manager's lookups (JADE_GET_VERTEX_SHADER, JADE_GET_PIXEL_SHADER).
//
// The map node of each key is remembered, so a repeated key reads its shader from the node instead of
// walking the guest std::map, about 7,000 walks a frame in a heavy scene
// (docs/development/draw-cost.md, "Shader lookup price").
//
// A cached node stays valid until JADE_SHADER_MANAGER_DESTROY: inserts never move nodes, and an
// insert on a present key replaces the shader inside the node, which is read again on every hit. A
// miss is never cached, since the original may then create the shader and insert it.
//
// Threading: after the renderer's setup (820E6E38, which also fills the maps from xeshader.sdb), the
// lookups and the loading queue's inserts all run on the render thread, and the destruction is the
// renderer's shutdown, so the walks here race with no insert. The tables are per thread anyway.

#include "game/addresses.h"
#include "memory.h"

#include <ppc_config.h>
#include <ppc_context.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

DECLARE_ORIGINAL(JADE_GET_VERTEX_SHADER);
DECLARE_ORIGINAL(JADE_GET_PIXEL_SHADER);
DECLARE_ORIGINAL(JADE_SHADER_MANAGER_DESTROY);

namespace game
{
    namespace
    {
        // Direct-mapped by a hash of the key and the map.
        constexpr uint32_t kCacheBits = 12;
        constexpr uint32_t kCacheEntries = 1u << kCacheBits;

        struct Entry
        {
            uint64_t key = 0;
            uint32_t map = 0; // 0: empty
            uint32_t node = 0;
        };

        // One cache per thread that looks shaders up, allocated on its first lookup, so a hit takes no
        // lock. A destruction bumps the generation, and a thread empties its cache when it sees a newer one.
        struct Cache
        {
            uint64_t generation = 0;
            std::array<Entry, kCacheEntries> entries{};
        };

        std::atomic<uint64_t> g_generation{ 0 };
        thread_local std::unique_ptr<Cache> t_cache;

        Entry& Slot(uint32_t map, uint64_t key)
        {
            uint64_t hash = (key ^ map) * 0x9E3779B97F4A7C15ull;
            return t_cache->entries[hash >> (64 - kCacheBits)];
        }

        // The node of key in the map, as the title's lookup finds it (lower_bound, then a check), or 0.
        uint32_t FindNode(uint32_t map, uint64_t key)
        {
            using namespace jade::std_map;
            uint32_t head = mem::Load32(map + kHead);
            uint32_t found = head;
            uint32_t node = mem::Load32(head + kParent); // the head's parent is the root
            while (mem::Load8(node + kIsNil) == 0)
            {
                if (mem::Load64(node + kKey) < key)
                    node = mem::Load32(node + kRight);
                else
                {
                    found = node;
                    node = mem::Load32(node + kLeft);
                }
            }
            if (found == head || key < mem::Load64(found + kKey))
                return 0;
            return found;
        }

        // The getters' contract: r3 the manager, r4 the key object; r3 = the shader, which may be 0.
        void GetShader(PPCContext& __restrict ctx, uint8_t* base, uint32_t mapOffset,
            void (*original)(PPCContext& __restrict, uint8_t*))
        {
            uint32_t keyObject = ctx.r4.u32;
            // A key class whose getter is not the plain one goes through the title's code.
            if (mem::Load32(mem::Load32(keyObject)) != jade::shader_key::kGetKey)
            {
                original(ctx, base);
                return;
            }
            uint32_t map = ctx.r3.u32 + mapOffset;
            uint64_t key = mem::Load64(keyObject + jade::shader_key::kKey);

            uint64_t generation = g_generation.load(std::memory_order_acquire);
            if (!t_cache)
                t_cache = std::make_unique<Cache>();
            if (t_cache->generation != generation)
            {
                t_cache->entries.fill(Entry{});
                t_cache->generation = generation;
            }
            Entry& entry = Slot(map, key);
            if (entry.map == map && entry.key == key)
            {
                ctx.r3.u64 = mem::Load32(entry.node + jade::std_map::kValue);
                return;
            }

            original(ctx, base);
            if (uint32_t node = FindNode(map, key))
                entry = Entry{ key, map, node };
        }
    }
}

// DX9_GetVertexShader: the lookup of a draw's vertex shader. Replaced to answer repeated keys from the
// cache; a new key runs the original (which creates the shader on a miss).
PPC_FUNC(JADE_GET_VERTEX_SHADER)
{
    game::GetShader(ctx, base, game::jade::shader_manager::kVertexMap, ORIGINAL(JADE_GET_VERTEX_SHADER));
}

// DX9_GetPixelShader: the same for the pixel shader.
PPC_FUNC(JADE_GET_PIXEL_SHADER)
{
    game::GetShader(ctx, base, game::jade::shader_manager::kPixelMap, ORIGINAL(JADE_GET_PIXEL_SHADER));
}

// The shader manager's destruction frees every node: hooked to empty the caches.
PPC_FUNC(JADE_SHADER_MANAGER_DESTROY)
{
    game::g_generation.fetch_add(1, std::memory_order_acq_rel);
    ORIGINAL(JADE_SHADER_MANAGER_DESTROY)(ctx, base);
    game::g_generation.fetch_add(1, std::memory_order_acq_rel);
}
