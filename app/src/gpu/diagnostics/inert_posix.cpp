// The diagnostics built on Windows only (crash_dump: minidumps; heap_watch:
// page protection with a vectored handler; sampler: suspended thread stacks;
// present_proof: the swapchain proof over window sizes) do nothing elsewhere;
// their switches are simply not read there.
#include "gpu/common/platform.h"
#include "gpu/d3d/d3d.h"
#include "gpu/diagnostics/crash_dump.h"
#include "gpu/diagnostics/heap_watch.h"
#include "gpu/diagnostics/sampler.h"

namespace gpu::crash_dump {
void Install() {}
void NoteFree(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
}  // namespace gpu::crash_dump

namespace gpu::heap_watch {
void Install() {}
bool Enabled() { return false; }
void NoteResolvedLock(bool, uint32_t) {}
void WriteReport(const char*) {}
}  // namespace gpu::heap_watch

namespace gpu::sampler {
bool Enabled() { return false; }
void RegisterThread(const char*) {}
void NoteSwap(uint64_t) {}
}  // namespace gpu::sampler

namespace gpu {
bool PresentProofRequested() { return false; }
void PresentProof(PPCContext&) { platform::Terminate(88); }
}  // namespace gpu
