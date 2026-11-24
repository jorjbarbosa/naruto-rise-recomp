#pragma once

// The shapes of the interface's movies that reach the edges of a wide picture (widescreen_shapes.cpp),
// for the movies widescreen.cpp centers.

#include <string_view>

namespace game::widescreen
{
    // A movie is about to be drawn in the 16:9 frame at the middle of a picture stretched that much:
    // until EndShapes, the shapes its rules choose are drawn over the whole picture instead. The thread
    // that draws the movie only.
    void BeginShapes(std::string_view movie, float stretch);
    void EndShapes();

    // True while such a shape is drawn on this thread: the records it submits stay out of the frame's
    // clip.
    bool DrawingWideShape();
}
