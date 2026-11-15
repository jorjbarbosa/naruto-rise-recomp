// Textures of the guest draws: how each fetch constant is sampled, and the uploads of the texture
// memory that changed since its last upload.

#include "gpu/guest/textures.h"

#include "gpu/gpu.h"
#include "gpu/guest/addresses.h"
#include "gpu/guest/resources.h"
#include "gpu/renderer/renderer.h"
#include "gpu/textures/layout.h"
#include "kernel/kernel.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

namespace gpu::guest
{
    namespace
    {
        constexpr uint32_t kFetchBytes = 24;
        // The fetch fields that place the texture in memory and give its texels meaning: pitch and
        // tiling, format, endian and base, size, mip range, dimension, packing and mip address.
        constexpr uint32_t kLayoutMask[6] = { 0xFFC00000, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0x000003FC, 0xFFFFFE00 };

        // DimensionField of a cube texture.
        constexpr uint32_t kDimensionCube = 3;

        // What was uploaded for a (GPU address, layout key), when its memory was last checked, and the
        // parts of its description a draw needs.
        struct Content
        {
            uint64_t hash = 0;
            uint64_t checkedFrame = 0; // 0: check at the next use
            bool uploaded = false;
            DrawTexture::Kind kind = DrawTexture::Kind::None;
            uint32_t mipMin = 0;
            uint32_t mipMax = 0;
        };

        std::mutex g_mutex; // the state below (the render thread binds, any thread unlocks)
        std::map<std::pair<uint32_t, uint64_t>, Content> g_contents; // ordered by address
        std::unordered_map<uint32_t, uint32_t> g_resolved;           // GPU address -> destination header
        uint64_t g_frame = 1;

        // A fetch constant with its base and mip addresses as GPU addresses (headers hold guest
        // addresses, the device the GPU ones).
        textures::Fetch ReadFetch(uint32_t address)
        {
            textures::Fetch fetch;
            for (uint32_t i = 0; i < 6; i++)
                fetch.d[i] = mem::Load32(address + 4 * i);
            if (fetch.BaseAddress() != 0)
                fetch.d[1] = GpuAddress(fetch.BaseAddress()) | (fetch.d[1] & 0xFFF);
            if (fetch.MipAddress() != 0)
                fetch.d[5] = GpuAddress(fetch.MipAddress()) | (fetch.d[5] & 0xFFF);
            return fetch;
        }

        uint64_t LayoutKey(const textures::Fetch& fetch)
        {
            uint32_t words[6];
            for (uint32_t i = 0; i < 6; i++)
                words[i] = fetch.d[i] & kLayoutMask[i];
            return XXH3_64bits(words, sizeof(words));
        }

        // The address a texture's host copy is known by: its base, or its mips without a base.
        uint32_t KeyAddress(const textures::Fetch& fetch)
        {
            return fetch.BaseAddress() != 0 ? fetch.BaseAddress() : fetch.MipAddress();
        }

        void LogUncovered(const char* reason, uint32_t format)
        {
            static std::mutex mutex;
            static std::set<std::pair<const char*, uint32_t>> logged;
            std::lock_guard lock(mutex);
            if (logged.insert({ reason, format }).second)
                Log("gpu: a texture (format %u) is not covered, it reads as zero: %s", format, reason);
        }

        // Fetch fields the host does not apply yet.
        void CheckUnapplied(const textures::Fetch& fetch)
        {
            for (uint32_t i = 0; i < 4; i++)
            {
                uint32_t select = (fetch.Swizzle() >> (3 * i)) & 7;
                if (select < 4 && ((fetch.Signs() >> (2 * select)) & 3) != 0)
                    LOG_ONCE("gpu: texture signs (signed, biased, gamma) are not applied yet; format %u", fetch.Format());
            }
            if (fetch.ExpAdjust() != 0)
                LOG_ONCE("gpu: the texture exponent adjustment is not applied yet");
        }

        uint64_t HashMemory(const std::vector<std::pair<uint32_t, uint32_t>>& ranges)
        {
            XXH3_state_t state;
            XXH3_64bits_reset(&state);
            for (const auto& [address, bytes] : ranges)
                XXH3_64bits_update(&state, mem::Ptr(GuestPhysical(address)), bytes);
            return XXH3_64bits_digest(&state);
        }

        // Marks the textures of an address to be checked at their next use. Under g_mutex.
        void Invalidate(uint32_t address)
        {
            for (auto it = g_contents.lower_bound({ address, 0 }); it != g_contents.end() && it->first.first == address; ++it)
                it->second.checkedFrame = 0;
        }
    }

    void ReadDrawTextures(uint32_t device, DrawOp& op)
    {
        Renderer* renderer = ActiveRenderer();
        // The uploads are posted under the lock, so a texture found checked is already posted.
        std::lock_guard lock(g_mutex);
        for (uint32_t slot = 0; slot < SharedConstants::kFetchSlots; slot++)
        {
            textures::Fetch fetch = ReadFetch(device + dev::kFetchConstants + kFetchBytes * slot);
            if (fetch.Type() != textures::kFetchTypeTexture)
                continue;
            DrawTexture& texture = op.textures[slot];
            texture.address = KeyAddress(fetch);
            texture.format = fetch.Format();
            texture.swizzle = fetch.Swizzle();
            texture.sampler.clamp = fetch.Clamp();
            texture.sampler.filter = fetch.Filter();
            texture.sampler.lodBias = fetch.LodBias();
            texture.sampler.border = fetch.BorderColor();
            if (texture.address != 0 && g_resolved.contains(texture.address))
            {
                texture.kind = fetch.DimensionField() == kDimensionCube ? DrawTexture::Kind::Cube : DrawTexture::Kind::Planar;
                texture.resolved = true;
                continue;
            }

            // A texture checked in this frame needs no description: its content holds the result.
            texture.layout = LayoutKey(fetch);
            auto it = g_contents.find({ texture.address, texture.layout });
            if (it != g_contents.end() && it->second.checkedFrame == g_frame)
            {
                texture.kind = it->second.kind;
                texture.mipMin = it->second.mipMin;
                texture.mipMax = it->second.mipMax;
                continue;
            }
            textures::Layout layout;
            const char* reason = nullptr;
            if (!textures::DescribeTexture(fetch, layout, reason))
            {
                texture = {};
                LogUncovered(reason, fetch.Format());
                continue;
            }
            CheckUnapplied(fetch);
            Content& content = it != g_contents.end() ? it->second : g_contents[{ texture.address, texture.layout }];
            content.kind = layout.dimension == textures::Dimension::kCube ? DrawTexture::Kind::Cube : DrawTexture::Kind::Planar;
            content.mipMin = layout.mipMin;
            content.mipMax = layout.mipMax;
            texture.kind = content.kind;
            texture.mipMin = content.mipMin;
            texture.mipMax = content.mipMax;

            std::vector<std::pair<uint32_t, uint32_t>> ranges = textures::MemoryRanges(layout);
            uint64_t hash = HashMemory(ranges);
            content.checkedFrame = g_frame;
            if (content.uploaded && hash == content.hash)
                continue;
            content.hash = hash;
            content.uploaded = true;

            auto upload = std::make_unique<TextureUpload>();
            upload->address = texture.address;
            upload->layoutKey = texture.layout;
            upload->layout = std::move(layout);
            for (const auto& [address, bytes] : ranges)
            {
                const uint8_t* memory = mem::Ptr(GuestPhysical(address));
                upload->data.insert(upload->data.end(), memory, memory + bytes);
            }
            upload->ranges = std::move(ranges);
            renderer->UploadTexture(std::move(upload));
        }
    }
    void NoteResolved(uint32_t address, uint32_t header)
    {
        std::lock_guard lock(g_mutex);
        g_resolved[address] = header;
    }

    void ForgetTexture(uint32_t header)
    {
        textures::Fetch fetch = ReadFetch(header + res::kTextureFetch);
        uint32_t base = fetch.BaseAddress();
        {
            std::lock_guard lock(g_mutex);
            auto resolved = g_resolved.find(base);
            if (resolved != g_resolved.end() && resolved->second == header)
                g_resolved.erase(resolved);
            for (uint32_t address : { base, fetch.MipAddress() })
            {
                auto first = g_contents.lower_bound({ address, 0 });
                auto last = first;
                while (last != g_contents.end() && last->first.first == address)
                    ++last;
                g_contents.erase(first, last);
            }
        }
        if (Renderer* renderer = ActiveRenderer())
            renderer->ReleaseTexture(KeyAddress(fetch), header);
    }

    void EndTextureFrame()
    {
        std::lock_guard lock(g_mutex);
        g_frame++;
    }
}

using namespace gpu::guest;

DECLARE_ORIGINAL(D3D_UNLOCK_RESOURCE);

// D3D::UnlockResource(resource, base, mip): decrements the lock count and, on the last unlock,
// flushes the CPU writes to the resource's memory. Replaced so that the textures of that memory are
// checked again at their next use, also within the frame.
PPC_FUNC(D3D_UNLOCK_RESOURCE)
{
    uint32_t resource = ctx.r3.u32;
    uint32_t common = mem::Load32(resource + res::kCommon);
    bool lastUnlock = ((common >> res::kLockCountShift) & res::kLockCountMask) == 1;
    ORIGINAL(D3D_UNLOCK_RESOURCE)(ctx, base);
    if (!lastUnlock || (common & res::kTypeMask) != res::kTypeTexture)
        return;
    gpu::textures::Fetch fetch = ReadFetch(resource + res::kTextureFetch);
    std::lock_guard lock(g_mutex);
    // The CPU wrote the memory through another header: it no longer holds the resolved image.
    auto resolved = g_resolved.find(fetch.BaseAddress());
    if (resolved != g_resolved.end() && resolved->second != resource)
        g_resolved.erase(resolved);
    Invalidate(fetch.BaseAddress());
    if (fetch.MipAddress() != 0)
        Invalidate(fetch.MipAddress());
}
