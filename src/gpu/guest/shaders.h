#pragma once

// The D3D shader objects of the title, registered when the Jade shader database creates them.

#include <cstdint>
#include <memory>
#include <vector>

namespace gpu::guest
{
    // A vertex element of a vertex shader: the fetch instruction that reads it and its semantic.
    // Its index in ShaderInfo::elements is the slot of its fetch descriptor.
    struct ShaderElement
    {
        uint32_t address = 0; // of the fetch instruction, in instructions (3 dwords each)
        uint8_t usage = 0;
        uint8_t usageIndex = 0;
    };

    // A shader object as it was created, before the D3D library patches anything in it.
    struct ShaderInfo
    {
        uint64_t key = 0; // the translated shader in the shader cache (XXH3-64 of the container)
        bool pixel = false;
        // Vertex shaders: the microcode (host order) with the fetch instructions of the shader
        // database, which the library patches in place for each vertex declaration, and the
        // vertex elements.
        std::vector<uint32_t> code;
        std::vector<ShaderElement> elements;
    };

    // The shader object at a guest address, or null when it was not created through the shader
    // database. Thread-safe.
    std::shared_ptr<const ShaderInfo> FindShader(uint32_t object);
}
