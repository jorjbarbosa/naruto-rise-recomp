// The title's entry thread on guest CPU 4.
//
// The runtime gives a thread created without an affinity the next guest CPU in
// turn, and the guest CPU is also the host core the thread is pinned to. The
// title's entry thread used to land on CPU 4 only because the SDK's audio
// created two host threads before it; the native renderer was tuned around
// that layout (its frame completion thread on CPU 2, its checks of
// VdInitializeEngines and VdSetGraphicsInterruptCallback on CPU 4). With the
// native audio those threads do not exist, so the entry thread is placed on
// CPU 4 here, whatever ran before it.
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/xthread.h>

extern "C" void __imp__xstart(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(xstart) {
  constexpr uint8_t kEntryCpu = 4;
  if (auto* thread = rex::system::XThread::GetCurrentThread(); thread && thread->main_thread()) {
    if (const uint8_t cpu = thread->active_cpu(); cpu != kEntryCpu) {
      REXLOG_INFO("[narutorise] Title entry thread moved from guest CPU {} to {}", cpu, kEntryCpu);
      thread->SetActiveCpu(kEntryCpu);
    }
  }
  __imp__xstart(ctx, base);
}
