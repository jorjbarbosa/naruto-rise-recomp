// The constant ring: the float constants of the draws on their way from the capture to the owner.

#include "gpu/renderer/constant_ring.h"

namespace gpu
{
    namespace
    {
        // Ranges start on a register.
        constexpr uint64_t kAlignment = kConstantRegisterBytes;
    }

    ConstantRing::ConstantRing(uint64_t capacity)
        : capacity_(capacity), data_(std::make_unique_for_overwrite<uint8_t[]>(capacity))
    {
    }

    std::optional<ConstantSpace> ConstantRing::TryReserve(uint32_t bytes)
    {
        uint64_t size = (uint64_t(bytes) + kAlignment - 1) & ~(kAlignment - 1);
        uint64_t position = head_;
        // A range does not wrap: one that would cross the end starts over at the beginning.
        uint64_t offset = position % capacity_;
        if (offset + size > capacity_)
            position += capacity_ - offset;
        if (size > capacity_ || position + size - Freed() > capacity_)
            return std::nullopt;
        head_ = position + size;
        return ConstantSpace{ { position, bytes }, data_.get() + position % capacity_ };
    }
}
