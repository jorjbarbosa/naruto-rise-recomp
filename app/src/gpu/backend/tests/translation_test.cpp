// Host test of the translation stage of the dispatcher with a fake
// translation. Never enters a guest function. Tasks of the stage (standing for
// draw captures) and the operations the title posts must reach the owner in
// one order (FIFO), a Run and a Drain from another thread pass through both
// stages, a task that asks for the stop makes the stage drop the discardable
// tasks after it while operations and runs still pass, and Shutdown closes in
// order with tasks pending (a pending Run fails, the translator is joined).
// Usage: native_engine_translation_test <new output.jsonl>
#include "gpu/backend/dispatcher.h"
#include "gpu/backend/tests/test_backend.h"
#include "gpu/diagnostics/sampler.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace gpu;

namespace gpu::sampler {
void RegisterThread(const char*) {}  // The sampler is not linked here.
}  // namespace gpu::sampler

namespace {
std::mutex order_mutex;
std::vector<int> owner_order;  // The order the owner ran the operations in.
std::function<bool(Engine&, std::string&)> Mark(int id) {
  return [id](Engine&, std::string&) {
    std::lock_guard lock(order_mutex);
    owner_order.push_back(id);
    return true;
  };
}
std::string Order() {
  std::lock_guard lock(order_mutex);
  std::string out;
  for (int id : owner_order) out += (out.empty() ? "" : ",") + std::to_string(id);
  return "[" + out + "]";
}
// A fake capture: translated on the translator, it posts its mark straight to
// the owner (as a translated draw is posted); optionally it asks for the stop.
class FakeCapture final : public StageTask, public DrawStorageAllocated<FakeCapture> {
 public:
  FakeCapture(Dispatcher& d, int id, bool stop = false, int sleep_ms = 0)
      : d_(d), id_(id), stop_(stop), sleep_ms_(sleep_ms) {}
  bool Discardable() const override { return true; }
  void Run() override {
    if (sleep_ms_) std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms_));
    if (!d_.OnTranslator()) return;  // Translated anywhere else: the mark is not posted (the FIFO check fails).
    d_.Post(Mark(id_));
    if (stop_) throw TranslationStopped{};
  }
 private:
  Dispatcher& d_;
  int id_;
  bool stop_;
  int sleep_ms_;
};
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 || std::filesystem::exists(argv[1])) return 2;
  std::ofstream output(argv[1]);
  if (!output) return 3;
  bool all = true;
  auto line = [&](const char* kind, bool ok, const std::string& evidence) {
    all = all && ok;
    output << "{\"kind\":\"" << kind << "\",\"ok\":" << (ok ? "true" : "false") << ",\"evidence\":"
           << (evidence.empty() ? "null" : evidence) << "}\n";
  };
  Dispatcher d(true, true);
  std::string evidence;
  const bool ready = d.Initialize(evidence, TestRenderMode());
  line("initialize", ready && d.Deferred() && !d.OnTranslator(), evidence);
  if (!ready) return 4;
  // FIFO: captures and operations from this thread reach the owner in the order given.
  d.Enqueue(std::make_unique<FakeCapture>(d, 1));
  d.Post(Mark(2));
  d.Enqueue(std::make_unique<FakeCapture>(d, 3));
  d.Defer([&] { d.Post(Mark(4)); });  // Work on the stage, in order with the captures.
  const bool ran = d.Run(Mark(5), evidence);  // Through both stages: everything before it has run.
  line("fifo-run", ran && Order() == "[1,2,3,4,5]", "{\"owner_order\":" + Order() + "}");
  const bool drained = d.Drain(evidence);
  line("drain", drained, evidence);
  // A guard of the translation: the captures after it are dropped, operations and runs pass.
  d.Enqueue(std::make_unique<FakeCapture>(d, 6, true));
  d.Enqueue(std::make_unique<FakeCapture>(d, 7));
  d.Post(Mark(8));
  const bool ran_after = d.Run(Mark(9), evidence);
  const std::string summary = "{" + d.StageSummary() + "}";
  line("discard-after-stop", ran_after && Order() == "[1,2,3,4,5,6,8,9]" &&
       summary.find("\"discarded\":1") != std::string::npos && summary.find("\"stops\":1") != std::string::npos,
       "{\"owner_order\":" + Order() + ",\"stage\":" + summary + "}");
  // Ordered close with tasks pending: a slow task in flight, a Run waiting
  // behind it from another thread, then Shutdown. The Run returns (done or
  // refused), the translator is joined, the engine goes.
  d.Enqueue(std::make_unique<FakeCapture>(d, 10, false, 300));
  bool run_returned = false, run_ok = false;
  std::string run_evidence;
  std::thread waiter([&] {
    run_ok = d.Run(Mark(11), run_evidence);
    run_returned = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const bool down = d.Shutdown(evidence);
  waiter.join();
  line("ordered-close", down && run_returned, "{\"run_ok\":" + std::string(run_ok ? "true" : "false") +
       ",\"run_evidence\":" + (run_evidence.empty() ? "null" : run_evidence) + ",\"shutdown\":" +
       (evidence.empty() ? "null" : evidence) + "}");
  // Force more than one local consume batch and interleave resource-like
  // operations. Then stop while a batch is in flight with a Run pending.
  {
    std::lock_guard lock(order_mutex);
    owner_order.clear();
  }
  Dispatcher batched(true, true);
  if (!batched.Initialize(evidence, TestRenderMode())) return 6;
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  batched.Defer([&] { entered.set_value(); released.wait(); });
  entered.get_future().wait();
  std::vector<int> expected;
  for (int i = 0; i < 96; ++i) {
    batched.Enqueue(std::make_unique<FakeCapture>(batched, i));
    expected.push_back(i);
    if (i == 16) { batched.Post(Mark(100)); expected.push_back(100); }
    if (i == 48) { batched.Defer([&] { batched.Post(Mark(101)); }); expected.push_back(101); }
  }
  release.set_value();
  const bool finished = batched.Run(Mark(999), evidence);
  expected.push_back(999);
  bool same_order = false;
  { std::lock_guard lock(order_mutex); same_order = owner_order == expected; }
  const std::string batched_summary = "{" + batched.StageSummary() + "}";
  line("batched-fifo", finished && same_order &&
       batched_summary.find("\"consume_batch_max\":32") != std::string::npos,
       "{\"owner_order\":" + Order() + ",\"stage\":" + batched_summary + "}");
  std::promise<void> entered_full, release_full;
  auto released_full = release_full.get_future().share();
  batched.Defer([&] { entered_full.set_value(); released_full.wait(); });
  entered_full.get_future().wait();
  for (size_t i = 0; i < Dispatcher::kStageMax; ++i)
    batched.Enqueue(std::make_unique<FakeCapture>(batched, 500 + int(i)));
  std::atomic<bool> full_returned{false};
  bool full_admitted = false;
  std::thread full_waiter([&] {
    full_admitted = batched.Enqueue(std::make_unique<FakeCapture>(batched, 9000));
    full_returned = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const bool blocked_when_full = !full_returned;
  release_full.set_value();
  full_waiter.join();
  const bool full_drained = batched.Run(Mark(9001), evidence);
  const std::string full_summary = "{" + batched.StageSummary() + "}";
  line("batched-backpressure", blocked_when_full && full_admitted && full_drained &&
       full_summary.find("\"space_notifications\":0") == std::string::npos, full_summary);
  std::promise<void> entered_close, release_close;
  auto released_close = release_close.get_future().share();
  batched.Defer([&] { entered_close.set_value(); released_close.wait(); });
  entered_close.get_future().wait();
  for (int i = 0; i < 96; ++i)
    batched.Enqueue(std::make_unique<FakeCapture>(batched, 200 + i, false, i == 0 ? 300 : 0));
  bool batch_run_returned = false;
  std::string batch_run_evidence;
  std::thread batch_waiter([&] { batched.Run(Mark(399), batch_run_evidence); batch_run_returned = true; });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  release_close.set_value();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const bool batch_down = batched.Shutdown(evidence);
  batch_waiter.join();
  line("batched-close", batch_down && batch_run_returned, evidence);
  output << "{\"kind\":\"verdict\",\"passed\":" << (all ? "true" : "false") << ",\"guest_executed\":false}\n";
  return all ? 0 : 5;
}
