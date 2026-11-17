#pragma once

// Readers of Jade engine structures shared by the state interpolation units. They only read guest
// memory; pointers are checked for null, not for being alive, so call them where the engine itself
// would read the same structures (the main thread, inside the main step).

#include "memory.h"

#include <bit>
#include <cstdint>
#include <vector>

namespace game::interpolation
{
    inline float LoadFloat(uint32_t address)
    {
        return std::bit_cast<float>(mem::Load32(address));
    }

    // The time structure, or 0 before the engine created it.
    uint32_t TimeStructure();

    // Whether an object passes the mount's filter (rendered unless invisible or culled) and has
    // active dynamics: a moving actor.
    bool HasDynamics(uint32_t object);

    // An object's additional matrices (its bones or gizmos): the guest address of each 68-byte matrix.
    std::vector<uint32_t> AdditionalMatrices(uint32_t object);

    // Whether an object has a modifier of the given type (the first word of its interface).
    bool HasModifier(uint32_t object, uint32_t type);
}
