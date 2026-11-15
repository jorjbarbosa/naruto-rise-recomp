// Vertex layouts: the D3D library's fetch patcher runs on a copy of the vertex shader's microcode,
// and the patched fetch instructions give each element's stream, offset and format.

#include "gpu/guest/vertex_input.h"

#include "gpu/guest/addresses.h"
#include "gpu/guest/shaders.h"
#include "kernel/kernel.h"

#include <algorithm>
#include <unordered_map>

DECLARE_ORIGINAL(D3D_PATCH_VERTEX_SHADER);

namespace gpu::guest
{
    namespace
    {
        constexpr uint32_t kMaxCodeBytes = 0x10000;
        constexpr uint32_t kStridesBytes = 16;
        constexpr uint32_t kVertexFetchConstants = 96;
        // D3DDECLUSAGE values (POSITION to SAMPLE), the semantics of the translated shaders' inputs.
        constexpr uint32_t kUsageCount = 14;
        // Stack the patcher may use below the frame of the hooked draw call.
        constexpr uint32_t kStackReserve = 1024;

        // Fields of a vertex fetch instruction (3 dwords).
        uint32_t Opcode(uint32_t w0) { return w0 & 0x1F; } // 0: vertex fetch
        uint32_t Destination(uint32_t w0) { return (w0 >> 12) & 0x7F; } // register and relative bit
        uint32_t FetchConstant(uint32_t w0) { return ((w0 >> 20) & 0x1F) * 3 + ((w0 >> 25) & 3); }
        bool IsMini(uint32_t w1) { return ((w1 >> 30) & 1) != 0; }
        uint32_t Format(uint32_t w1) { return (w1 >> 16) & 0x3F; }
        uint32_t Stride(uint32_t w2) { return w2 & 0xFF; }  // dwords
        uint32_t Offset(uint32_t w2) { return (w2 >> 8) & 0x7FFFFF; } // dwords

        // The components a fetch writes: destination selector 7 keeps the register's component.
        uint32_t WrittenMask(uint32_t w1)
        {
            uint32_t mask = 0;
            for (uint32_t i = 0; i < 4; i++)
                if (((w1 >> (3 * i)) & 7) != 7)
                    mask |= 1u << i;
            return mask;
        }

        // Dwords of a vertex format (GPUVERTEXFORMAT), 0 for the formats vfetch does not decode.
        uint32_t FormatDwords(uint32_t format)
        {
            switch (format)
            {
            case 6: case 7: case 16: case 17: case 25: case 31: case 33: case 36:
                return 1;
            case 26: case 32: case 34: case 37:
                return 2;
            case 57:
                return 3;
            case 35: case 38:
                return 4;
            default:
                return 0;
            }
        }

        // The descriptor of a fetch instruction, without the endian swap (vfetch in
        // tools/XenosRecomp/XenosRecomp/shader_common.h, patches/XenosRecomp/0005).
        uint32_t Descriptor(uint32_t w1)
        {
            return Format(w1) | ((w1 >> 12) & 1) << 6 | ((w1 >> 13) & 1) << 7 | ((w1 >> 14) & 1) << 8 |
                (w1 & 0xFFF) << 11 | ((w1 >> 24) & 0x3F) << 23;
        }

        uint64_t Mix(uint64_t hash, uint64_t value)
        {
            hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
            return hash;
        }

        struct Key
        {
            uint32_t object = 0;
            uint64_t shader = 0;
            uint32_t declaration = 0;
            uint32_t declarationId = 0;
            std::array<uint8_t, 16> strides{};

            bool operator==(const Key&) const = default;
        };

        struct KeyHash
        {
            size_t operator()(const Key& key) const
            {
                uint64_t hash = Mix(Mix(Mix(key.object, key.shader), key.declaration), key.declarationId);
                for (uint8_t stride : key.strides)
                    hash = Mix(hash, stride);
                return size_t(hash);
            }
        };

        std::unordered_map<Key, std::shared_ptr<const VertexLayout>, KeyHash> g_layouts;
        uint32_t g_scratch = 0;

        // The microcode with its fetch instructions patched for the declaration and strides, as the
        // shader commit of a draw patches it.
        std::vector<uint32_t> Patch(PPCContext& ctx, uint8_t* base, uint32_t object, const ShaderInfo& shader,
            uint32_t declaration, const std::array<uint8_t, 16>& strides)
        {
            if (g_scratch == 0)
            {
                g_scratch = mem::Alloc(mem::Heap::System, kMaxCodeBytes + kStridesBytes, 16);
                if (g_scratch == 0)
                    Fatal("gpu: out of system memory for the vertex fetch patch");
            }
            uint32_t code = g_scratch;
            uint32_t stridesAddress = g_scratch + kMaxCodeBytes;
            for (size_t i = 0; i < shader.code.size(); i++)
                mem::Store32(code + uint32_t(4 * i), shader.code[i]);
            memcpy(mem::Ptr(stridesAddress), strides.data(), kStridesBytes);

            PPCContext saved = ctx;
            ctx.r1.u64 = (ctx.r1.u32 - kStackReserve) & ~15u;
            ctx.r3.u64 = object;
            ctx.r4.u64 = code;
            ctx.r5.u64 = declaration;
            ctx.r6.u64 = stridesAddress;
            ctx.r7.u64 = 0; // the shader itself, not the variant for draws without a pixel shader
            ORIGINAL(D3D_PATCH_VERTEX_SHADER)(ctx, base);
            ctx = saved;

            std::vector<uint32_t> patched(shader.code.size());
            for (size_t i = 0; i < patched.size(); i++)
                patched[i] = mem::Load32(code + uint32_t(4 * i));
            return patched;
        }

        std::shared_ptr<const VertexLayout> Build(const ShaderInfo& shader, const std::vector<uint32_t>& code)
        {
            // The fetches at the element addresses in address order. The patch can reorder them
            // (mini fetches follow the full fetch of their stream), so the fetch of an element is
            // the one that writes its register; a mini fetch takes the constant and the stride of
            // the full fetch before it.
            struct Fetch
            {
                uint32_t w0, w1, w2, constant, stride;
            };
            std::vector<uint32_t> addresses;
            for (const ShaderElement& element : shader.elements)
                addresses.push_back(element.address);
            std::sort(addresses.begin(), addresses.end());
            addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());

            std::vector<Fetch> fetches;
            uint32_t constant = ~0u;
            uint32_t stride = 0;
            for (uint32_t address : addresses)
            {
                if (3 * size_t(address) + 2 >= code.size() || Opcode(code[3 * address]) != 0)
                {
                    LOG_ONCE("gpu: a vertex element does not point at a vertex fetch; draws with it are skipped");
                    return nullptr;
                }
                Fetch fetch{ code[3 * address], code[3 * address + 1], code[3 * address + 2], 0, 0 };
                if (!IsMini(fetch.w1))
                {
                    constant = FetchConstant(fetch.w0);
                    stride = Stride(fetch.w2);
                }
                if (constant == ~0u)
                {
                    LOG_ONCE("gpu: a mini vertex fetch has no full fetch before it; draws with it are skipped");
                    return nullptr;
                }
                fetch.constant = constant;
                fetch.stride = stride;
                fetches.push_back(fetch);
            }

            auto layout = std::make_shared<VertexLayout>();
            auto input = std::make_shared<VertexInput>();
            for (const ShaderElement& element : shader.elements)
            {
                if (element.usage >= kUsageCount)
                {
                    LOG_ONCE("gpu: vertex element usage %u has no semantic; draws with it are skipped", element.usage);
                    return nullptr;
                }
                uint32_t w0 = shader.code[3 * element.address];
                uint32_t w1 = shader.code[3 * element.address + 1];
                const Fetch* match = nullptr;
                uint32_t matches = 0;
                for (const Fetch& fetch : fetches)
                    if (Destination(fetch.w0) == Destination(w0))
                    {
                        match = &fetch;
                        matches++;
                    }
                // Two fetches into one register write different components.
                if (matches > 1)
                {
                    matches = 0;
                    for (const Fetch& fetch : fetches)
                        if (Destination(fetch.w0) == Destination(w0) && WrittenMask(fetch.w1) == WrittenMask(w1))
                        {
                            match = &fetch;
                            matches++;
                        }
                }
                if (matches != 1)
                {
                    LOG_ONCE("gpu: a vertex element matches %u patched fetches; draws with it are skipped", matches);
                    return nullptr;
                }

                uint32_t dwords = FormatDwords(Format(match->w1));
                uint32_t offset = Offset(match->w2);
                if (dwords == 0)
                {
                    LOG_ONCE("gpu: vertex format %u is not covered; draws with it are skipped", Format(match->w1));
                    return nullptr;
                }
                // A stride of 0 reads the same vertex for every index.
                if (match->constant >= kVertexFetchConstants || (match->stride != 0 && offset + dwords > match->stride))
                {
                    LOG_ONCE("gpu: a vertex element (fetch %u, stride %u, offset %u) is not covered; draws with it are "
                        "skipped", match->constant, match->stride, offset);
                    return nullptr;
                }

                auto it = std::find(layout->streamFetch.begin(), layout->streamFetch.end(), match->constant);
                uint32_t stream = uint32_t(it - layout->streamFetch.begin());
                if (it == layout->streamFetch.end())
                {
                    layout->streamFetch.push_back(match->constant);
                    input->strides.push_back(match->stride * 4);
                }
                else if (input->strides[stream] != match->stride * 4)
                {
                    LOG_ONCE("gpu: two vertex elements read one stream with different strides; draws with them are "
                        "skipped");
                    return nullptr;
                }
                input->elements.push_back({ element.usage, element.usageIndex, uint8_t(stream), uint8_t(dwords), offset * 4 });
                layout->descriptors.push_back(Descriptor(match->w1));
            }

            uint64_t id = 0;
            for (const VertexElement& element : input->elements)
                id = Mix(Mix(id, element.usage | element.usageIndex << 8 | element.stream << 16 | element.dwords << 24),
                    element.offset);
            for (uint32_t streamStride : input->strides)
                id = Mix(id, streamStride);
            input->id = id;
            layout->input = std::move(input);
            return layout;
        }
    }

    std::shared_ptr<const VertexLayout> FindVertexLayout(PPCContext& ctx, uint8_t* base, uint32_t shaderObject,
        const ShaderInfo& shader, uint32_t declaration, const std::array<uint8_t, 16>& strides)
    {
        // Each declaration gets an id at its first patch; one without an id yet is not cached, since
        // its address may later hold another declaration.
        Key key{ shaderObject, shader.key, declaration, declaration != 0 ? mem::Load32(declaration + res::kDeclarationId) : 0, strides };
        bool cached = declaration == 0 || key.declarationId != 0;
        if (cached)
        {
            auto it = g_layouts.find(key);
            if (it != g_layouts.end())
                return it->second;
        }

        if (shader.code.size() * 4 > kMaxCodeBytes)
        {
            LOG_ONCE("gpu: a vertex shader of %zu bytes is larger than the patch area; its draws are skipped",
                shader.code.size() * 4);
            return nullptr;
        }
        // As the shader commit: without a declaration, or with the container flag, the fetches stay
        // as the shader database has them.
        bool unpatched = declaration == 0 ||
            (mem::Load32(shaderObject + shader::kVertexContainer) & shader::kFlagUnpatched) != 0;
        std::shared_ptr<const VertexLayout> layout =
            Build(shader, unpatched ? shader.code : Patch(ctx, base, shaderObject, shader, declaration, strides));
        if (cached)
            g_layouts.emplace(key, layout);
        return layout;
    }
}
