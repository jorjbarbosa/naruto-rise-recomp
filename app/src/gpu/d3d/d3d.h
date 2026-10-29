#pragma once

// The D3D boundary of the native renderer seen from the host.
#include <memory>
#include <string>
#include <string_view>
#include <rex/ppc/context.h>
#include "gpu/host/services.h"

// Entry points of the D3D boundary that the host calls. Hooks owned only by
// the native mode live in the units of the boundary; hooks shared with the
// earlier altitude (host/host.cpp) call these first and
// return when Active().
namespace gpu {
class FailureChannel;
bool Active();
// The device of the 8213ECE8 in progress, before its arena (VdInitializeEngines
// is called there); 0 outside that window.
uint32_t EnteredDevice();
void DeviceArena(PPCContext& ctx, uint8_t* base);        // 8212FA80 inside 8213ECE8
void DeviceWorkers(PPCContext& ctx, uint8_t* base);      // 821480D0
void DeviceCache(PPCContext& ctx, uint8_t* base);        // 82131428
void DeviceInitializer(PPCContext& ctx, uint8_t* base);  // 821483D0
void DeviceVideo(PPCContext& ctx, uint8_t* base);        // 8213E8F0
void DeviceDefaults(PPCContext& ctx, uint8_t* base);     // 8213E610
void DeviceMemory(PPCContext& ctx, uint8_t* base);       // 8217DDB0 at LR 8213EE0C, runs the original
void RingReserve(PPCContext& ctx, uint8_t* base);        // 8212F608: flush omitted or guard
bool AdmitExport(std::string_view name, PPCContext& ctx);
void ExportAfter(std::string_view name, PPCContext& ctx);  // Records admitted queries.
// True when a packet-writing leaf (8212F7E0) runs inside an own discard scope
// of this thread with its cursor in the discard buffer.
bool AdmitDiscardLeaf(PPCContext& ctx);
// True when 8212F608 is the space reserve of the 821083B8 tail (LR 82108E24)
// inside its own scope; r3 then holds the own discard buffer.
bool AdmitSceneFlushReserve(PPCContext& ctx, uint8_t* base);
// Asks the render thread to stop in order (StopInOrder) at its next
// own Swap or draw. False when the native mode has no live device.
bool RequestStop(std::string_view reason, int code);
// The own interrupt source flipped the frame of this fence; publishes
// it as the completed fence (DeviceOwner::PublishFrame), authorized by the
// session's failure channel inside the device's fence lock: refused once the
// channel holds a failure (FailureChannel::Authorize).
enum class FramePublication { kPublished, kRefused, kFailed };
FramePublication PublishFrameFence(uint32_t fence, FailureChannel& failures);
// Posts the Present of a frame at its flip, before its fence is
// published. False when it could not be posted.
bool PresentFrame(const std::shared_ptr<host::FramePresent>& present);
std::string PresentSummary();
// REX_NATIVE_PRESENT_PROOF=1 proves the own swapchain on the SDK
// window right after the engine is ready, before any title frame, and stops.
bool PresentProofRequested();
[[noreturn]] void PresentProof(PPCContext& ctx);
// The ordered close on the window close (UI thread). The render
// thread parks at its next own Swap or draw (10 s, else the fallback claims the
// stop and the render thread parks behind it); then the own interrupt thread
// is joined, the title terminated and its guest threads joined, and the device
// owner, the registry resources, the swapchain and the engine are released.
bool CloseInOrder();
}  // namespace gpu
