#pragma once

#include "gpu/renderer/renderer.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

namespace gpu
{
    // Host memory that carries the float constants of one shader stage from the thread that captures
    // the draws (the producer) to the owner thread (the consumer). Ranges are handed out at growing
    // positions, and the owner frees by position: once it has drawn a draw, the space before that
    // draw's range is free, since the draws posted after it name the same range or newer ones
    // (Renderer::ReserveConstants). TryReserve and the space it hands out: one producer thread at a
    // time. Data and Free: owner thread only. The posting of the draws orders the producer's writes
    // before the owner's reads, and Free orders the owner's reads before the producer reuses the
    // space.
    class ConstantRing
    {
    public:
        explicit ConstantRing(uint64_t capacity);

        // Space for bytes after the last range handed out, or none while the owner still needs the
        // space it would take.
        std::optional<ConstantSpace> TryReserve(uint32_t bytes);
        // How far the owner has freed; a change means TryReserve may now succeed. Any thread.
        uint64_t Freed() const { return freed_.load(std::memory_order_acquire); }

        // The constants of a range handed out and not freed.
        const uint8_t* Data(const ConstantRange& range) const { return data_.get() + range.position % capacity_; }
        // The space before position, the start of a range, is free. A position below the last one
        // freed changes nothing.
        void Free(uint64_t position)
        {
            if (position > freed_.load(std::memory_order_relaxed))
                freed_.store(position, std::memory_order_release);
        }

    private:
        uint64_t capacity_;
        std::unique_ptr<uint8_t[]> data_;
        uint64_t head_ = 0; // producer: where the next range goes
        std::atomic<uint64_t> freed_{ 0 };
    };
}
