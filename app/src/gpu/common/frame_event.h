#pragma once

// The host event of a frame lot: the Swap creates it, Engine::CloseFrame sets
// it once every operation submitted so far completed, and the frame completion
// thread (host::QueueFrameEnd) waits for it. A manual-reset event of the SDK,
// shared by whoever may still set or wait for it; on Windows its native handle
// is the kernel event that the D3D12 fence sets.
#include <memory>
#include <rex/thread.h>

namespace gpu {
using FrameEvent = std::shared_ptr<rex::thread::Event>;
inline FrameEvent CreateFrameEvent() { return rex::thread::Event::CreateManualResetEvent(false); }
}  // namespace gpu
