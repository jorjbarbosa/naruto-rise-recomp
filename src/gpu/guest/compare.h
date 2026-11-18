#pragma once

// The game side of the comparison harness (scripts/compare-gpu.ps1): a run that reaches a scene and
// records some of its frames, which gpu/replay.h then draws on either graphics API.
//
// - ROAN_SCRIPT=Return@600,Space@640,S@700: keys (SDL names) held for kScriptHoldFrames frames from
//   the given frame (frames counted in Swaps from 1), to reach a scene without a hand on the keys.
// - ROAN_RECORD_DIR=<folder> and ROAN_RECORD_FRAMES=900,1500: those frames are recorded as
//   frame-<n>.rxf (Renderer::RecordNextFrame). The run quits after the last one.
//
// Off without those variables. Called from the thread that posts the guest's GPU work.

#include <cstdint>

namespace gpu
{
    class Renderer;
}

namespace gpu::guest
{
    // A frame was presented: presses and lets go of the script's keys, and starts or ends the frame
    // recordings.
    void EndCompareFrame(Renderer* renderer);
}
