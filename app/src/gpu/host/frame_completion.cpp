// The frame completion thread of the native mode.
#include "gpu/host/internal.h"
#include "gpu/backend/dispatcher.h"
#include "gpu/common/platform.h"
#include "gpu/d3d/d3d.h"
#include "gpu/diagnostics/census.h"
#include <deque>
#include <optional>
#include <vector>
#include <fmt/format.h>
#include <rex/system/flags.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

namespace gpu::host {
// The frame completion source of the D3D boundary. The own Swap presents at
// once and paces itself with a limiter on the title's interval; no interrupt
// is delivered to the title (no vblank, no sync interrupt 82138358 of
// 821384F0, no flip), and nothing outside the graphics library reads the
// vblank or flip counters. One XHostThread (CPU 2) waits for the own lot of
// each Swap, in order, and publishes its frame fence on the writeback block
// as soon as the own queue completed it, as the console GPU writes the fence
// at the end of the frame's commands.
namespace frame_completion {
struct FrameEnd {
  uint32_t fence = 0;
  FrameEvent event;
  std::vector<gpu::host::OwnInterrupt> interrupts;  // Always empty.
  uint64_t swap = 0;
  int64_t queued_qpc = 0;
  std::shared_ptr<gpu::host::FramePresent> present;  // Posted by the Swap.
};
struct CompletionRecord {
  int64_t qpc, completed_qpc, queued_qpc;
  uint32_t fence;
  uint64_t swap;
};
thread_local bool on_thread = false;
std::atomic<bool> started{false}, closing{false}, frames_refused{false};
std::atomic<uint64_t> queued{0}, frame_ends{0};
std::atomic<int64_t> max_publish_after_lot_qpc{0};
std::mutex queue_mutex;
std::deque<FrameEnd> frames;
std::unique_ptr<rex::thread::Event> doorbell;  // Auto reset: a queued frame end or the stop.
uint32_t device = 0;
int64_t qpc_frequency = 0, qpc_start = 0;
// Completion timings (pacing_mutex), QPC ticks; bounded.
constexpr size_t kPacingRecordsMax = size_t(1) << 18;
std::mutex pacing_mutex;
std::vector<CompletionRecord> completion_records;
// Stops new publications and wakes the thread.
void Quiesce() {
  closing.store(true);
  if (doorbell) doorbell->Set();
}
std::string Summary() {
  size_t pending = 0;
  {
    std::lock_guard lock(queue_mutex);
    pending = frames.size();
  }
  return fmt::format("{{\"started\":{},\"mode\":\"frame-completion\",\"frame_ends_queued\":{},\"frame_ends\":{},"
                     "\"pending\":{},\"frames_refused\":{},\"max_publish_after_lot_us\":{},\"vblanks\":0,"
                     "\"interrupts_delivered\":0}}", started.load(), queued.load(), frame_ends.load(), pending,
                     frames_refused.load(),
                     qpc_frequency ? max_publish_after_lot_qpc.load() * 1000000 / qpc_frequency : 0);
}

constexpr uint8_t kCpu = 2;
rex::system::object_ref<rex::system::XHostThread> owner;  // Lives until process exit.

// The host tick clock (the QPC on Windows), kept under its old names in the records.
int64_t Qpc() { return platform::Ticks(); }
// The own lot of this frame end completed on the own queue: its fence becomes
// the completed value, unless the session's failure channel refuses it
// (false: a failure was retained since the caller checked). A Present carried
// by the frame end (none: the Swap posts its own) is posted first.
bool PublishFrame(const FrameEnd& frame, int64_t completed_qpc, gpu::FailureChannel& failures) {
  if (frame.present && !gpu::PresentFrame(frame.present) && !closing.load())
    Stop("own-frame-present-post", Context(), frame.fence, uint32_t(frame.swap), 88);
  const auto publication = gpu::PublishFrameFence(frame.fence, failures);
  if (publication == gpu::FramePublication::kRefused) return false;
  if (publication != gpu::FramePublication::kPublished)
    Stop("own-frame-end-publication", Context(), frame.fence, uint32_t(frame.swap), 88);
  const int64_t now = Qpc();
  const uint64_t n = frame_ends.fetch_add(1) + 1;
  if (now - completed_qpc > max_publish_after_lot_qpc.load()) max_publish_after_lot_qpc.store(now - completed_qpc);
  {
    std::lock_guard lock(pacing_mutex);
    if (completion_records.size() < kPacingRecordsMax)
      completion_records.push_back({now, completed_qpc, frame.queued_qpc, frame.fence, frame.swap});
  }
  if (n <= 8 || n % 300 == 0)
    Record("native-own-frame-end", fmt::format(
        "\"frame_end\":{},\"swap\":{},\"fence\":{},\"lot_us\":{},\"published_after_lot_us\":{},\"mode\":"
        "\"frame-completion\"", n, frame.swap, frame.fence, (completed_qpc - frame.queued_qpc) * 1000000 / qpc_frequency,
        (now - completed_qpc) * 1000000 / qpc_frequency));
  return true;
}
int Run() {
  on_thread = true;
  if (!rex::runtime::ThreadState::Get()) Stop("own-completion-thread-context", Context(), 0, 0, 88);
  {
    std::lock_guard lock(event_mutex);
    Record("native-own-completion-thread", "XHostThread-CPU2-frame-completion", Context(), device, 0);
  }
  std::optional<FrameEnd> frame;  // The frame end in progress, in queue order.
  for (;;) {
    if (closing.load()) break;
    if (!frame) {
      std::lock_guard lock(queue_mutex);
      if (!frames.empty()) {
        frame = std::move(frames.front());
        frames.pop_front();
      }
    }
    const bool lot_pending = frame && !frames_refused.load();
    std::vector<rex::thread::WaitHandle*> handles{doorbell.get()};
    if (lot_pending) handles.push_back(frame->event.get());
    const auto wait = rex::thread::WaitAny(handles, false, std::chrono::milliseconds(1000)).first;
    if (closing.load()) break;
    if (wait == rex::thread::WaitResult::kFailed) Stop("own-completion-wait", Context(), 0, 0, 88);
    if (!lot_pending ||
        rex::thread::Wait(frame->event.get(), false, std::chrono::milliseconds(0)) != rex::thread::WaitResult::kSuccess)
      continue;
    const int64_t completed_qpc = Qpc();
    frame->event.reset();
    // Nothing is published once the session's failure channel holds a
    // failure: checked here, and again under the channel at the fence store.
    std::string failure;
    auto* engine = gpu::host::ActiveDispatcher();
    bool published = false;
    if (engine && !engine->PostedFailure(failure)) {
      // Not covered: without the census the frame is published without them.
      if (!frame->interrupts.empty() && !gpu::NoteUncoveredOperation("own-frame-end-sync-uncovered"))
        Stop("own-frame-end-sync-uncovered", Context(), frame->fence, uint32_t(frame->interrupts.size()), 86);
      published = PublishFrame(*frame, completed_qpc, engine->Failures());
      if (!published) engine->PostedFailure(failure);  // Retained in between.
    }
    if (!published) {
      frames_refused.store(true);
      Record("native-own-frame-end-refused", fmt::format("\"fence\":{},\"swap\":{},\"evidence\":{}",
                    frame->fence, frame->swap, failure.empty() ? "null" : failure));
      continue;
    }
    frame.reset();
  }
  return 0;
}
// After Quiesce, waits (bounded) for the completion thread to exit
// (Run returns, XHostThread::Execute calls Exit).
bool Join(uint32_t ms, std::string& fields) {
  auto* host = owner ? owner->thread() : nullptr;
  const auto wait = host ? rex::thread::Wait(host, false, std::chrono::milliseconds(ms)) : rex::thread::WaitResult::kFailed;
  fields = fmt::format("\"started\":{},\"closing\":{},\"thread\":{},\"wait\":{},\"joined\":{}", started.load(),
                       closing.load(), owner ? owner->thread_id() : 0, int(wait), wait == rex::thread::WaitResult::kSuccess);
  return wait == rex::thread::WaitResult::kSuccess;
}
bool Start(uint32_t device_address, uint32_t, std::string& fields) {
  if (started.load() || closing.load() || !Kernel() || !InitializerRegistration(device_address))
    return false;
  device = device_address;
  qpc_frequency = platform::TickFrequency();
  if (qpc_frequency <= 0) return false;
  doorbell = rex::thread::Event::CreateAutoResetEvent(false);
  if (!doorbell) return false;
  qpc_start = Qpc();
  owner = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
      Kernel(), 128 * 1024, rex::system::X_CREATE_SUSPENDED | ((1u << kCpu) << 24),
      [] { return Run(); }));
  owner->set_name("NativeOwnFrameCompletionCPU2");
  if (owner->Create() != 0) return false;
  owner->SetAffinity(1u << kCpu);
  started.store(true);
  if (owner->Resume() != 0) return false;
  fields = fmt::format("\"mode\":\"frame-completion\",\"device\":{},\"cpu\":{},\"vblank\":null,"
                       "\"interrupts\":false,\"qpc_frequency\":{},\"guest_thread\":{},\"legacy_graphics_system\":false",
                       device, kCpu, qpc_frequency, owner->thread_id());
  return true;
}
bool QueueFrame(uint32_t fence, FrameEvent event, std::vector<gpu::host::OwnInterrupt> interrupts, uint64_t swap,
                std::shared_ptr<gpu::host::FramePresent> present) {
  if (!started.load() || closing.load() || !event) return false;
  {
    std::lock_guard lock(queue_mutex);
    frames.push_back(FrameEnd{fence, std::move(event), std::move(interrupts), swap, Qpc(), std::move(present)});
  }
  queued.fetch_add(1);
  doorbell->Set();
  return true;
}
std::string PacingJson() {
  std::string text;
  {
    std::lock_guard lock(pacing_mutex);
    text = fmt::format("{{\"schema\":\"native-arq-frame-completion-v1\",\"qpc_frequency\":{},\"qpc_start\":{},"
                       "\"frame_end_columns\":\"qpc,completed_qpc,queued_qpc,fence,swap\",\"frame_ends\":[",
                       qpc_frequency, qpc_start);
    for (size_t i = 0; i < completion_records.size(); ++i) {
      const auto& f = completion_records[i];
      text += fmt::format("{}[{},{},{},{},{}]", i ? "," : "", f.qpc, f.completed_qpc, f.queued_qpc, f.fence, f.swap);
    }
  }
  return text + "],\"summary\":" + Summary() + "}";
}
}  // namespace frame_completion

bool StartInterrupts(uint32_t device, uint32_t scratch, std::string& fields) {
  return frame_completion::Start(device, scratch, fields);
}
bool QueueFrameEnd(uint32_t fence, FrameEvent lot_event, std::vector<OwnInterrupt> interrupts, uint64_t swap,
                   std::shared_ptr<FramePresent> present) {
  return frame_completion::QueueFrame(fence, std::move(lot_event), std::move(interrupts), swap, std::move(present));
}
void QuiesceInterrupts() { frame_completion::Quiesce(); }
bool JoinInterrupts(uint32_t ms, std::string& fields) {
  if (!frame_completion::started.load()) {
    fields = "\"started\":false,\"joined\":true";
    return true;
  }
  return frame_completion::Join(ms, fields);
}
bool InterruptsClosing() { return frame_completion::closing.load(); }
std::string InterruptSummary() { return frame_completion::Summary(); }
std::string PacingJson() { return frame_completion::PacingJson(); }
}  // namespace gpu::host
