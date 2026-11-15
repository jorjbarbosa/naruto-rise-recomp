// Textures of the guest draws: how each fetch constant is sampled, and the uploads of the texture
// memory that changed since its last upload.

#include "gpu/guest/textures.h"

#include "gpu/gpu.h"
#include "gpu/guest/addresses.h"
#include "gpu/guest/resources.h"
#include "gpu/renderer/renderer.h"
#include "gpu/textures/formats.h"
#include "gpu/textures/layout.h"
#include "kernel/kernel.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <cstring>
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
        // tiling, format, endian and base, size, border, mip range, dimension, packing and mip
        // address. Equal fields give an equal layout, so a content keeps the layout it was described
        // with.
        constexpr uint32_t kLayoutMask[6] = { 0xFFC00000, 0xFFFFFFFF, 0xFFFFFFFF, 0x80000000, 0x000003FC, 0xFFFFFE00 };

        // DimensionField of a cube texture.
        constexpr uint32_t kDimensionCube = 3;

        // GPU memory pages (mem::kPageSize) of the physical heap.
        constexpr uint32_t kPageShift = 12;
        constexpr uint32_t kPhysicalPages = 0x20000000 >> kPageShift;

        // A (GPU address, layout key): its layout, the memory it reads, and the write check its last
        // upload copied after.
        struct Content
        {
            textures::Layout layout;
            std::vector<std::pair<uint32_t, uint32_t>> ranges; // GPU address, bytes
            DrawTexture::Kind kind = DrawTexture::Kind::None;
            uint64_t uploadCheck = 0;  // 0: never uploaded
            uint64_t checkedFrame = 0; // 0: check at the next use
        };

        std::mutex g_mutex; // the state below (the render thread binds, any thread unlocks or frees)
        std::map<std::pair<uint32_t, uint64_t>, Content> g_contents; // ordered by address
        // A resolve into texture memory: the destination header, and the number of the resolve.
        struct Resolve
        {
            uint32_t header = 0;
            uint64_t number = 0;
        };
        std::unordered_map<uint32_t, Resolve> g_resolved;            // by GPU address
        uint64_t g_resolves = 0;
        uint64_t g_frame = 1;
        // Write checks. The OS reports a written page to one check only, and contents share pages
        // (several layouts over the same memory), so each page keeps the number of the last check
        // that found it written: a content is stale when a page of it was found written after the
        // check its upload copied after.
        uint64_t g_check = 0;
        std::vector<uint64_t> g_pageChecks(kPhysicalPages);

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

        // The ranges of a layout that lie in the physical heap, which is all the GPU addresses: a
        // fetch can describe memory past its end, which nothing can have written.
        std::vector<std::pair<uint32_t, uint32_t>> PhysicalRanges(const textures::Layout& layout)
        {
            constexpr uint64_t kEnd = uint64_t(kPhysicalPages) << kPageShift;
            std::vector<std::pair<uint32_t, uint32_t>> ranges;
            for (const auto& [address, bytes] : textures::MemoryRanges(layout))
            {
                uint64_t end = std::min<uint64_t>(uint64_t(address) + bytes, kEnd);
                if (end > address)
                    ranges.emplace_back(address, uint32_t(end - address));
            }
            return ranges;
        }

        // A new write check of a content's ranges (PhysicalRanges): the pages written since the last
        // check that covered them get its number, check. Returns whether a page of the ranges was found
        // written after the check `since`. A copy of the memory made after the check holds every
        // write it found; later writes are found by later checks. Under g_mutex.
        bool CheckWrites(const std::vector<std::pair<uint32_t, uint32_t>>& ranges, uint64_t since, uint64_t& check)
        {
            thread_local std::vector<uint32_t> written;
            check = ++g_check;
            bool stale = false;
            for (const auto& [address, bytes] : ranges)
            {
                uint32_t first = address >> kPageShift;
                uint32_t last = (address + bytes - 1) >> kPageShift;
                written.clear();
                if (!mem::TakeWrittenPages(GuestPhysical(address), bytes, written))
                {
                    // Without the OS's answer, every page counts as written.
                    std::fill(g_pageChecks.begin() + first, g_pageChecks.begin() + last + 1, check);
                    stale = true;
                    continue;
                }
                for (uint32_t page : written)
                    g_pageChecks[(page - GuestPhysical(0)) >> kPageShift] = check;
                stale |= !written.empty();
                // Pages another content's check found written after this content's upload.
                for (uint32_t page = first; !stale && page <= last; page++)
                    stale = g_pageChecks[page] > since;
            }
            return stale;
        }

        // Forgets the contents of the GPU addresses in [begin, end). Under g_mutex.
        void EraseContents(uint32_t begin, uint32_t end)
        {
            auto first = g_contents.lower_bound({ begin, 0 });
            auto last = first;
            while (last != g_contents.end() && last->first.first < end)
                ++last;
            g_contents.erase(first, last);
        }

        // Marks the textures of an address to be checked at their next use. Under g_mutex.
        void Invalidate(uint32_t address)
        {
            for (auto it = g_contents.lower_bound({ address, 0 }); it != g_contents.end() && it->first.first == address; ++it)
                it->second.checkedFrame = 0;
        }

        // A resolve leaves its image in the destination's memory on the console; here the image stays
        // on the host until the CPU locks that memory. Writes the resolved region of level 0 back in
        // the layout of the header's fetch constant (resolves write no other level), leaving the
        // texels no resolve wrote as they are. The memory then holds the image, so the address stops
        // sampling the host texture: a CPU write through the lock must reach the draws. Any thread;
        // blocks until the GPU is idle.
        void WriteBackResolved(uint32_t header)
        {
            textures::Fetch fetch = ReadFetch(header + res::kTextureFetch);
            uint32_t address = fetch.BaseAddress();
            uint64_t number = 0;
            {
                std::lock_guard lock(g_mutex);
                auto it = address != 0 ? g_resolved.find(address) : g_resolved.end();
                if (it == g_resolved.end())
                    return;
                number = it->second.number;
            }

            // The image, read back before the lock: the readback waits for the GPU.
            Renderer* renderer = ActiveRenderer();
            textures::Layout layout;
            const char* reason = nullptr;
            ResolvedImage image;
            std::vector<uint8_t> colors;
            const textures::Format* format = textures::FindFormat(fetch.Format());
            bool ready = renderer != nullptr && format != nullptr && textures::DescribeTexture(fetch, layout, reason) &&
                layout.hasBase && renderer->ReadResolved(address, image) && image.width == layout.width &&
                image.height == layout.height && textures::PackColors(fetch.Format(), fetch.Swizzle(), image.rgba, colors);

            textures::GuestLevel level;
            uint8_t* memory = nullptr;
            if (ready)
            {
                const textures::LevelPlace& place = layout.levels[0];
                memory = mem::Ptr(GuestPhysical(place.address));
                level.memory = memory;
                level.bytes = place.extent;
                level.tiled = layout.tiled;
                level.rowPitch = place.rowPitch;
                level.endian = layout.endian;
                level.x0 = place.x0;
                level.y0 = place.y0;
                level.widthBlocks = image.width;
                level.heightBlocks = image.height;
                level.bytesPerBlock = format->bytesPerBlock;
            }

            // Under the lock, so that no upload copies the memory halfway through.
            std::lock_guard lock(g_mutex);
            std::vector<uint8_t> blocks;
            if (ready && textures::ReadLevel(level, blocks))
            {
                uint32_t bytes = level.bytesPerBlock;
                const Rect& region = image.region;
                for (int32_t y = region.top; y < region.bottom; y++)
                    memcpy(&blocks[(size_t(y) * image.width + region.left) * bytes],
                        &colors[(size_t(y) * image.width + region.left) * bytes], size_t(region.right - region.left) * bytes);
                ready = textures::WriteLevel(level, memory, blocks);
            }
            else
            {
                ready = false;
            }
            if (!ready)
            {
                LOG_ONCE("gpu: a resolve the CPU locks (format %u) is not written back; the CPU reads the old memory",
                    fetch.Format());
                return;
            }
            // A resolve noted since the readback is newer than what was written: it stays.
            auto it = g_resolved.find(address);
            if (it != g_resolved.end() && it->second.number == number)
                g_resolved.erase(it);
            Invalidate(address);
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

            // A content is described once; its layout follows from its key.
            texture.layout = LayoutKey(fetch);
            auto it = g_contents.find({ texture.address, texture.layout });
            if (it == g_contents.end())
            {
                textures::Layout layout;
                const char* reason = nullptr;
                if (!textures::DescribeTexture(fetch, layout, reason))
                {
                    texture = {};
                    LogUncovered(reason, fetch.Format());
                    continue;
                }
                CheckUnapplied(fetch);
                it = g_contents.emplace(std::pair(texture.address, texture.layout), Content()).first;
                Content& content = it->second;
                content.kind = layout.dimension == textures::Dimension::kCube ? DrawTexture::Kind::Cube
                                                                               : DrawTexture::Kind::Planar;
                content.ranges = PhysicalRanges(layout);
                content.layout = std::move(layout);
            }
            Content& content = it->second;
            texture.kind = content.kind;
            texture.mipMin = content.layout.mipMin;
            texture.mipMax = content.layout.mipMax;
            if (content.checkedFrame == g_frame)
                continue;

            content.checkedFrame = g_frame;
            uint64_t check = 0;
            if (!CheckWrites(content.ranges, content.uploadCheck, check) && content.uploadCheck != 0)
                continue;
            content.uploadCheck = check;

            auto upload = std::make_unique<TextureUpload>();
            upload->address = texture.address;
            upload->layoutKey = texture.layout;
            upload->layout = content.layout;
            upload->ranges = content.ranges;
            for (const auto& [address, bytes] : content.ranges)
            {
                const uint8_t* memory = mem::Ptr(GuestPhysical(address));
                upload->data.insert(upload->data.end(), memory, memory + bytes);
            }
            renderer->UploadTexture(std::move(upload));
        }
    }
    void NoteResolved(uint32_t address, uint32_t header)
    {
        std::lock_guard lock(g_mutex);
        g_resolved[address] = { header, ++g_resolves };
    }

    void ForgetTexture(uint32_t header)
    {
        textures::Fetch fetch = ReadFetch(header + res::kTextureFetch);
        uint32_t base = fetch.BaseAddress();
        {
            std::lock_guard lock(g_mutex);
            auto resolved = g_resolved.find(base);
            if (resolved != g_resolved.end() && resolved->second.header == header)
                g_resolved.erase(resolved);
            for (uint32_t address : { base, fetch.MipAddress() })
                EraseContents(address, address + 1);
        }
        if (Renderer* renderer = ActiveRenderer())
            renderer->ReleaseTexture(KeyAddress(fetch), header);
    }

    void ForgetMemory(uint32_t address, uint32_t size)
    {
        uint32_t begin = GpuAddress(address);
        uint32_t end = begin + size;
        // A texture can keep its base and its mips in different allocations: every content that
        // reads the freed memory goes, with the other layouts of its address (they share the host
        // textures released by address).
        std::set<uint32_t> outside;
        {
            std::lock_guard lock(g_mutex);
            for (const auto& [key, content] : g_contents)
            {
                if (key.first >= begin && key.first < end)
                    continue;
                for (const auto& [rangeAddress, bytes] : content.ranges)
                    if (rangeAddress < end && rangeAddress + bytes > begin)
                        outside.insert(key.first);
            }
            EraseContents(begin, end);
            for (uint32_t key : outside)
                EraseContents(key, key + 1);
            std::erase_if(g_resolved, [&](const auto& entry) { return entry.first >= begin && entry.first < end; });
        }
        Renderer* renderer = ActiveRenderer();
        if (renderer == nullptr)
            return;
        renderer->ReleaseMemory(begin, end);
        // Header 0 made no resolve: only the uploaded textures of those addresses go.
        for (uint32_t key : outside)
            renderer->ReleaseTexture(key, 0);
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
    if (resolved != g_resolved.end() && resolved->second.header != resource)
        g_resolved.erase(resolved);
    Invalidate(fetch.BaseAddress());
    if (fetch.MipAddress() != 0)
        Invalidate(fetch.MipAddress());
}

DECLARE_ORIGINAL(D3D_LOCK_SURFACE);

// D3D::LockSurface(texture, level, face, flags, data*, pitch*, ..., offset*): the address of a level
// and face for a CPU lock; every LockRect reaches it. Replaced so that a resolve's image is in the
// memory before the CPU reads it (the save thumbnail, D3DDevice_PersistDisplay's front buffer).
PPC_FUNC(D3D_LOCK_SURFACE)
{
    uint32_t texture = ctx.r3.u32;
    if (ctx.r4.u32 == 0 && ctx.r5.u32 == 0 && (mem::Load32(texture + res::kCommon) & res::kTypeMask) == res::kTypeTexture)
        WriteBackResolved(texture);
    ORIGINAL(D3D_LOCK_SURFACE)(ctx, base);
}
