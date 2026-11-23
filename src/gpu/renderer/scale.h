#pragma once

#include "gpu/renderer/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gpu
{
    // The title's one video mode (kernel/video.cpp), which is also the size of its largest surface
    // (step 0 of the image quality plan found nothing larger): the scale is lowered so that the
    // mode fits the device's textures.
    constexpr uint32_t kVideoWidth = 1280;
    constexpr uint32_t kVideoHeight = 720;

    // The internal resolution scale. The host makes every guest surface and resolved texture this
    // many times larger in each dimension, while the operations keep guest pixels: Targets and Draws
    // map them here. Fixed for the life of the renderer; a value type, used on the owner thread.
    class Scale
    {
    public:
        explicit Scale(float factor = 1.0f) : factor_(factor) {}

        float Factor() const { return factor_; }

        // A guest pixel boundary in host pixels. Each edge of a rectangle is mapped alone, so that
        // rectangles that touch in guest pixels still touch at a fractional scale.
        int32_t Edge(int32_t guest) const { return int32_t(std::lround(double(guest) * factor_)); }

        // A guest size in host pixels: the edge it ends at, never 0.
        uint32_t Size(uint32_t guest) const { return std::max(uint32_t(Edge(int32_t(guest))), 1u); }

        Rect Map(const Rect& guest) const
        {
            return { Edge(guest.left), Edge(guest.top), Edge(guest.right), Edge(guest.bottom) };
        }

        // A viewport coordinate: a whole guest pixel maps as an edge, so that the viewport meets the
        // rectangles; a fractional one linearly.
        float Coordinate(float guest) const
        {
            return guest == std::floor(guest) ? float(Edge(int32_t(guest))) : guest * factor_;
        }

        // The host columns at the left of a surface, and rows at its top, that the title's draws leave
        // uncovered. The host moves every position by half a guest pixel (the D3D9 pixel centers,
        // SharedConstants::halfPixelOffset), which is factor / 2 host pixels: the pixel centers
        // closer than that to the edge are covered only by the title's own full-screen quads, which
        // shift the other way. None at 1x, where the top-left rule covers the first center.
        int32_t Gap() const { return std::max(int32_t(std::ceil(factor_ / 2.0f - 0.5f)), 0); }

    private:
        float factor_;
    };
}
