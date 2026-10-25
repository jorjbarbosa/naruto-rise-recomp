#pragma once

#include <rex/ppc/context.h>
#include "gpu/common/frame_event.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace gpu { class Dispatcher; }
namespace rex::memory { class Memory; }
namespace rex::system { class KernelState; }

// Services that the host of the native mode (host/host.cpp)
// exports to the units of the D3D boundary: the journal, the stop path, the
// engine and the frame completion thread.
namespace gpu::host {
bool Enabled();
// The journal is open (the diagnostic cases). The product writes none: its
// stop and close summaries go to the SDK log instead.
bool Journaled();
[[noreturn]] void Stop(std::string_view reason, PPCContext& ctx, int code);
// Claims the single ordered stop; a thread arriving while another
// owns it records its reason and parks until the process exits.
void ClaimStop(std::string_view reason, PPCContext& ctx);
void Record(std::string_view kind, std::string_view fields);
// Fields of the live START provenance that the stop writes, with the
// journal, executable and source identity hashes, after the last journal line.
void SetStopProvenance(std::string_view fields);
bool Read(rex::memory::Memory* memory, uint32_t address, void* target, size_t size);
Dispatcher* ActiveDispatcher();
bool InitializerRegistration(uint32_t device);
// The own frame completion thread. No interrupt is delivered to the title.
bool StartInterrupts(uint32_t device, uint32_t scratch, std::string& fields);
// One frame end of the own Swap, in order: the thread waits for the lot event
// (the own queue completed the frame) and publishes the frame fence through
// gpu::PublishFrameFence. interrupts must be empty and present null (the
// Swap posts its Present). Takes ownership of lot_event.
struct OwnInterrupt { uint32_t routine, argument, cpu_mask; };
// What the flip of a frame end presents: the stage slot that the
// Swap's copy of its front buffer took on the engine owner (written there by
// the copy, read there by the Present posted at the flip). Null presents nothing.
struct FramePresent {
  uint32_t front = 0, slot = UINT32_MAX;
  uint64_t swap = 0;
  bool capture = false;  // Back buffer read back (the Swap read its front buffer too).
};
bool QueueFrameEnd(uint32_t fence, FrameEvent lot_event, std::vector<OwnInterrupt> interrupts, uint64_t swap,
                   std::shared_ptr<FramePresent> present);
void QuiesceInterrupts();
bool InterruptsClosing();
// Ordered close: join of the own interrupt thread after QuiesceInterrupts
// (bounded), the stop claimed without parking, the threads parked behind the
// stop owner, the kernel and the engine release of a stop without its exit.
bool JoinInterrupts(uint32_t ms, std::string& fields);
bool TryClaimStop();
uint32_t ParkedStops();
rex::system::KernelState* Kernel();
bool ReleaseEngineForClose(std::string_view reason);
std::string InterruptSummary();
// Vblank and flip timings of the own source (integers only), for the
// measurement file.
std::string PacingJson();
// Totals of the high-frequency records (guest waits, SetLastError)
// whose individual lines are capped in the native mode.
std::string RecordSummary();
}  // namespace gpu::host
