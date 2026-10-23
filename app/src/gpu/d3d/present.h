#pragma once

// Presentation layer of the D3D boundary (d3d/present.cpp): the
// own swapchain on the SDK window, the title's gamma ramp and the Present of
// each own Swap.
#include <cstdint>
#include <memory>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include <rex/ppc/context.h>
#include "gpu/common/platform.h"
#include "gpu/d3d/d3d.h"

namespace rex::ui { class Window; }

namespace gpu {
rex::ui::Window* DisplayWindow();
// The SDK window by the handles of the system (gpu/common/platform.h).
platform::NativeWindow DisplayNativeWindow();
// Runs function on the UI thread and waits for it.
bool InUiThread(std::function<void()> function);
// A file of REX_NATIVE_FRAME_DIR, as JSON fields (file, sha256).
std::string PresentFile(const std::string& name, const std::vector<uint8_t>& bytes);
// REX_NATIVE_CLOSE_AFTER_SWAPS, the window close without a person.
void MaybePostClose(uint64_t swap);
// First own Swap: the swapchain on the SDK window, before the first frame end.
void AttachPresentation(PPCContext& ctx);
// The ramp of the title when SetGammaRamp marked it pending (bit 1 of +16692).
void PostGammaRamp(PPCContext& ctx, uint8_t* base, uint32_t dev);
// The copy of the front buffer for this Swap's flip, in the frame lot.
std::shared_ptr<host::FramePresent> PostStageFront(PPCContext& ctx, uint32_t front, uint64_t swap,
                                                               bool capture);
}  // namespace gpu
