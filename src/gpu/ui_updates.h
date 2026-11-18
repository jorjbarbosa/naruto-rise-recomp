#pragma once

// What the UI sent since the last present (ui.h), as the renderer's owner takes it.

#include "gpu/ui.h"

#include <memory>
#include <utility>
#include <vector>

namespace gpu::ui
{
    struct Updates
    {
        // Applied in this order: the creates, the draw of the frame, then the releases.
        std::vector<std::pair<Texture, TextureDesc>> creates;
        std::shared_ptr<const Frame> frame; // null: the frame did not change
        std::vector<Texture> releases;
    };

    // Everything sent since the last call. Any thread; the renderer's owner calls it at each present.
    Updates TakeUpdates();
}
