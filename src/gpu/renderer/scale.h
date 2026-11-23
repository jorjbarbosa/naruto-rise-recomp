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

    // The internal resolution scale, per axis. The host makes every guest surface and resolved
    // texture this many times larger, while the operations keep guest pixels: Targets and Draws map
    // them here. The axes differ in a window wider than 16:9, where the guest squeezes its picture
    // horizontally and the host renders it that much wider. A value type, used on the owner thread.
    class Scale
    {
    public:
        Scale() = default;
        Scale(float x, float y) : x_(x), y_(y) {}

        float X() const { return x_; }
        float Y() const { return y_; }
        bool operator==(const Scale&) const = default;

        // A guest pixel boundary in host pixels. Each edge of a rectangle is mapped alone, so that
        // rectangles that touch in guest pixels still touch at a fractional scale.
        int32_t EdgeX(int32_t guest) const { return Edge(guest, x_); }
        int32_t EdgeY(int32_t guest) const { return Edge(guest, y_); }

        // A guest size in host pixels: the edge it ends at, never 0.
        uint32_t Width(uint32_t guest) const { return std::max(uint32_t(EdgeX(int32_t(guest))), 1u); }
        uint32_t Height(uint32_t guest) const { return std::max(uint32_t(EdgeY(int32_t(guest))), 1u); }

        Rect Map(const Rect& guest) const
        {
            return { EdgeX(guest.left), EdgeY(guest.top), EdgeX(guest.right), EdgeY(guest.bottom) };
        }

        // A viewport coordinate: a whole guest pixel maps as an edge, so that the viewport meets the
        // rectangles; a fractional one linearly.
        float CoordinateX(float guest) const { return Coordinate(guest, x_); }
        float CoordinateY(float guest) const { return Coordinate(guest, y_); }

        // The host columns at the left of a surface, and rows at its top, that the title's draws leave
        // uncovered. The host moves every position by half a guest pixel (the D3D9 pixel centers,
        // SharedConstants::halfPixelOffset), which is scale / 2 host pixels: the pixel centers
        // closer than that to the edge are covered only by the title's own full-screen quads, which
        // shift the other way. None at 1x, where the top-left rule covers the first center.
        int32_t GapX() const { return Gap(x_); }
        int32_t GapY() const { return Gap(y_); }

        // The factor of the slope-scaled depth bias. The slope is per guest pixel and a host pixel
        // spans 1 / scale of the depth a guest pixel did; the larger axis, so that the bias never
        // falls short of the title's.
        float Slope() const { return std::max(x_, y_); }

    private:
        static int32_t Edge(int32_t guest, float factor) { return int32_t(std::lround(double(guest) * factor)); }
        static float Coordinate(float guest, float factor)
        {
            return guest == std::floor(guest) ? float(Edge(int32_t(guest), factor)) : guest * factor;
        }
        static int32_t Gap(float factor) { return std::max(int32_t(std::ceil(factor / 2.0f - 0.5f)), 0); }

        float x_ = 1.0f;
        float y_ = 1.0f;
    };
}
