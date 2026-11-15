#pragma once

// The vertex input of the draws: what each vertex shader input reads, for the bound vertex
// declaration and stream strides.

#include "gpu/renderer/renderer.h"

#include <ppc_context.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace gpu::guest
{
    struct ShaderInfo;

    // The vertex fetch constant 95 - stream holds the vertex buffer of a stream.
    constexpr uint32_t kStreamFetchBase = 95;

    // The input of a vertex shader as the D3D library patches its fetch instructions for a vertex
    // declaration and the stream strides: which fetch constant each stream reads, and the format of
    // each element. Shared by the draws with the same shader, declaration and strides.
    struct VertexLayout
    {
        std::shared_ptr<const VertexInput> input;
        std::vector<uint32_t> streamFetch; // the vertex fetch constant of each stream
        // The fetch descriptor of each element (SharedConstants::vertexFetch) without the endian
        // swap, which comes from the fetch constant of its stream at draw time.
        std::vector<uint32_t> descriptors;
    };

    // The endian swap field of a fetch descriptor.
    constexpr uint32_t kDescriptorEndianShift = 9;

    // The layout of a vertex shader object for the declaration and strides (dwords per stream).
    // The first time, the library's patcher (82140540) runs on a copy of the shader's microcode,
    // with ctx (the context of the draw hook) to run guest code. Null when the layout is not covered
    // (logged once per reason). Render thread only.
    std::shared_ptr<const VertexLayout> FindVertexLayout(PPCContext& ctx, uint8_t* base, uint32_t shaderObject,
        const ShaderInfo& shader, uint32_t declaration, const std::array<uint8_t, 16>& strides);
}
