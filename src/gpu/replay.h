#pragma once

// The replay of a recorded frame (gpu/renderer/recording.h) instead of the game: ROAN_REPLAY=<file>,
// on the API of ROAN_GPU_API. The textures and buffers live when the frame started are uploaded,
// every surface the frame uses is cleared (color to zero, depth to 1 and stencil to 0) so that both
// APIs start from the same surfaces, the operations are posted in their order, and the front buffer
// and every color texture the frame resolved are read back into output as front.ppm and
// resolve-<address>.ppm. scripts/compare-gpu.ps1 diffs the outputs of the two APIs.

#include <filesystem>

namespace gpu
{
    // Host main thread, with the window created. 0 when the frame was replayed and its images
    // written; 1 otherwise (logged).
    int RunReplay(const std::filesystem::path& file, const std::filesystem::path& output);
}
