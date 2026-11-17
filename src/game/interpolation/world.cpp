// Readers of Jade engine structures for the state interpolation units (world.h).

#include "game/interpolation/world.h"

#include "game/addresses.h"

namespace game::interpolation
{
    namespace
    {
        // A bound that keeps a damaged structure from turning into a huge walk.
        constexpr uint32_t kMaxAdditionalMatrices = 1024;
        constexpr uint32_t kMaxModifiers = 64;
        constexpr uint32_t kDynamicsActive = 0x80000000;
    }

    uint32_t TimeStructure()
    {
        return mem::Load32(jade::kTime);
    }

    bool HasDynamics(uint32_t object)
    {
        uint32_t status = mem::Load32(object + jade::object::kStatus);
        if ((status & jade::object::kStatusMounted) == 0 || (status & jade::object::kStatusNotMounted) != 0)
            return false;
        uint32_t base = mem::Load32(object + jade::object::kBase);
        uint32_t dynamics = base != 0 ? mem::Load32(base + jade::base::kDynamics) : 0;
        return dynamics != 0 && (mem::Load32(dynamics) & kDynamicsActive) != 0;
    }

    std::vector<uint32_t> AdditionalMatrices(uint32_t object)
    {
        std::vector<uint32_t> matrices;
        uint32_t base = mem::Load32(object + jade::object::kBase);
        uint32_t additional = base != 0 ? mem::Load32(base + jade::base::kAdditionalMatrices) : 0;
        if (additional == 0)
            return matrices;
        uint32_t count = mem::Load32(additional + jade::additional::kCount);
        uint32_t array = mem::Load32(additional + jade::additional::kArray);
        if (array == 0 || count > kMaxAdditionalMatrices)
            return matrices;
        bool pointers = (mem::Load32(object + jade::object::kIdentity) & jade::object::kIdentityAdditionalPointers) != 0;
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t matrix = pointers ? mem::Load32(array + i * jade::gizmo::kSize + jade::gizmo::kMatrix)
                                       : array + i * jade::matrix::kSize;
            if (matrix != 0)
                matrices.push_back(matrix);
        }
        return matrices;
    }

    bool HasModifier(uint32_t object, uint32_t type)
    {
        uint32_t extended = mem::Load32(object + jade::object::kExtended);
        uint32_t modifier = extended != 0 ? mem::Load32(extended + jade::extended::kModifiers) : 0;
        for (uint32_t n = 0; modifier != 0 && n < kMaxModifiers; n++)
        {
            uint32_t face = mem::Load32(modifier + jade::modifier::kInterface);
            if (face != 0 && mem::Load32(face) == type)
                return true;
            modifier = mem::Load32(modifier + jade::modifier::kNext);
        }
        return false;
    }
}
