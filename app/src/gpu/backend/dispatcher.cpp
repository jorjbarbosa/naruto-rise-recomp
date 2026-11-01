#include "gpu/backend/dispatcher.h"
#include "gpu/backend/lifecycle.h"
#include "gpu/backend/pipeline_set.h"
#include "diagnostics/incident.h"
#include "diagnostics/report_context.h"
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <atomic>
#include <array>
#include <algorithm>
#include <fmt/format.h>
#include "gpu/common/platform.h"
#include "gpu/common/title_loading.h"
#include <rex/thread.h>
#include "gpu/diagnostics/profile.h"
#include "gpu/diagnostics/sampler.h"

namespace gpu {
struct Dispatcher::State {
  State(bool dedicated, bool deferred) : dedicated(dedicated), deferred(deferred) {}
  bool dedicated, deferred;
  bool initialized = false, closing = false, stopped = false;
  bool frontend_retained = false;
  uint64_t next_request = 1;
  // Monotonic identity of this owner for the process; zero is never issued.
  std::mutex mutex;
  std::mutex lifecycle;
  std::condition_variable wake;
  // The session's failure channel, shared with the engine.
  const std::shared_ptr<FailureChannel> failures = std::make_shared<FailureChannel>();
  // Posted operations (mutex).
  size_t posted_pending = 0;
  uint64_t posted = 0, posted_dropped = 0;
  std::condition_variable posted_space;
  // The open draw batch (mutex) and its counters.
  std::vector<DrawRequest> draw_batch;
  uint64_t draw_batches = 0, draws_batched = 0, batch_flush_full = 0, batch_flush_operation = 0,
           batch_flush_run = 0, batch_flush_close = 0, batch_max = 0;
  std::deque<std::function<void()>> work;
  std::thread owner;
  std::unique_ptr<Engine> engine;
  // The precompile pool (StartPool), joined by Shutdown first.
  std::vector<std::unique_ptr<rex::thread::Thread>> pool;
  std::atomic<size_t> pool_next{0};
  std::atomic<unsigned> pool_active{0};
  std::atomic<bool> pool_stop{false};
  // The translation stage (stage_mutex): its FIFO, the translator thread
  // and its counters. discarding: a guard asked for the stop.
  std::mutex stage_mutex;
  std::condition_variable stage_wake, stage_space;
  std::deque<std::unique_ptr<StageTask>> stage;
  std::atomic<bool> stage_stopped{false};
  std::thread translator;
  std::atomic<uint32_t> translator_id{0};
  std::atomic<bool> discarding{false};
  uint64_t stage_enqueued = 0, stage_waits = 0, stage_max = 0;
  size_t stage_waiting = 0;
  // The translator is waiting on stage_wake: only then an enqueue wakes it
  // (the queue is also empty while it runs the batch it took).
  bool translator_sleeping = false;
  uint64_t stage_notifications = 0, stage_space_notifications = 0;
  uint64_t stage_consume_batches = 0, stage_consume_batch_max = 0;
  std::atomic<uint64_t> stage_depth_frame{0};  // The deepest since the last take.
  std::atomic<uint64_t> stage_run{0}, stage_discarded{0}, stage_stops{0};

  void Start() {
    owner = std::thread([this] {
      rex::thread::set_current_thread_name("Native Engine Owner");
      sampler::RegisterThread("engine");  // REX_NATIVE_SAMPLE_PERIOD_US.
      narutorise::report_context::SetThreadRole("engine");
      for (;;) {
        std::function<void()> task;
        {
          std::unique_lock lock(mutex);
          wake.wait(lock, [&] { return !work.empty() || stopped; });
          if (stopped) return;
          task = std::move(work.front());
          work.pop_front();
        }
        narutorise::incident::Activity activity(narutorise::incident::Role::kGpu);
        task();
        narutorise::incident::Progress(narutorise::incident::Role::kGpu);
      }
    });
  }
  // The translator consumes the stage in order. A task that throws
  // TranslationStopped asked the title to stop; the discardable tasks after it
  // are dropped, the others still pass. At the stop the pending tasks are
  // destroyed (a pending Run fails in its destructor).
  void StartStage() {
    translator = std::thread([this] {
      translator_id.store(rex::thread::current_thread_system_id());
      rex::thread::set_current_thread_name("Native Translation");
      sampler::RegisterThread("translate");
      narutorise::report_context::SetThreadRole("translate");
      for (;;) {
        // Take only work already queued; no timer or minimum batch delays a
        // draw. A single consumer keeps captures and operations in FIFO order.
        std::array<std::unique_ptr<StageTask>, 32> batch;
        size_t count = 0;
        bool notify_space = false;
        {
          std::unique_lock lock(stage_mutex);
          while (stage.empty() && !stage_stopped) {
            translator_sleeping = true;
            stage_wake.wait(lock);
            translator_sleeping = false;
          }
          if (stage_stopped) {
            stage.clear();
            stage_space.notify_all();
            return;
          }
          count = std::min(batch.size(), stage.size());
          for (size_t i = 0; i < count; ++i) {
            batch[i] = std::move(stage.front());
            stage.pop_front();
          }
          ++stage_consume_batches;
          stage_consume_batch_max = std::max<uint64_t>(stage_consume_batch_max, count);
          notify_space = stage_waiting != 0;
          if (notify_space) ++stage_space_notifications;
        }
        if (notify_space) stage_space.notify_all();
        for (size_t i = 0; i < count; ++i) {
          // Shutdown finishes the running task, drops the rest of this local
          // batch and clears the FIFO on the next iteration. Pending Runs
          // are destroyed too, fulfilling their refusal promises.
          if (stage_stopped.load(std::memory_order_relaxed)) break;
          auto task = std::move(batch[i]);
          if (discarding.load(std::memory_order_acquire) && task->Discardable()) {
            stage_discarded.fetch_add(1);
            continue;
          }
          stage_run.fetch_add(1);
          try {
            narutorise::incident::Activity activity(narutorise::incident::Role::kTranslator);
            task->Run();
            narutorise::incident::Progress(narutorise::incident::Role::kTranslator);
          } catch (const TranslationStopped&) {
            discarding.store(true, std::memory_order_release);
            stage_stops.fetch_add(1);
          }
        }
      }
    });
  }
  void StopStage() {
    {
      std::lock_guard lock(stage_mutex);
      stage_stopped = true;
    }
    stage_wake.notify_all();
    stage_space.notify_all();
    if (translator.joinable()) translator.join();
  }
  void Queue(std::function<void()> task) {
    work.push_back(std::move(task)); // Caller holds mutex.
    wake.notify_one();
  }
  // One posted operation, admitted under the mutex (lock held by
  // the caller; the wait for room releases it). Shared by Post and the
  // draw batches.
  bool PostLocked(std::unique_lock<std::mutex>& lock, std::function<bool(Engine&, std::string&)> operation) {
    if (!dedicated || !initialized || closing || stopped || failures->Failed()) return false;
    posted_space.wait(lock, [&] { return posted_pending < Dispatcher::kPostedMax || failures->Failed() || stopped; });
    if (failures->Failed() || stopped) return false;
    ++posted_pending;
    const uint64_t sequence = ++posted;
    Queue([this, sequence, operation = std::move(operation)] {
      // After a failure the engine itself refuses every operation; a posted
      // one is dropped unexecuted. The first failure is retained in the
      // channel (outside the dispatcher's lock: its wakers take it).
      const bool drop = !engine || failures->Failed();
      std::string raw;
      bool ok = true;
      if (!drop) {
        profile::Timer engine_time(profile::kEngineOperation);
        ok = operation(*engine, raw);
        if (!ok)
          failures->Retain({sequence, FailureChannel::kPostedOperation,
                            raw.empty() ? "{\"reason\":\"engine-missing\"}" : raw});
      }
      std::lock_guard guard(mutex);
      if (drop) ++posted_dropped;
      --posted_pending;
      posted_space.notify_all();
    });
    return true;
  }
  // Posts the open draw batch (lock held). The wait for room comes
  // first, so the batch taken is the one open when room exists and no later
  // draw of the same thread can pass it. False only when the batch could not
  // be admitted (closed or failed dispatcher): its draws are dropped, as any
  // posted operation after a failure.
  enum class Flush { Full, Operation, Run, Close };
  bool FlushDrawsLocked(std::unique_lock<std::mutex>& lock, Flush why) {
    if (draw_batch.empty()) return true;
    if (!dedicated || !initialized || closing || stopped || failures->Failed()) { draw_batch.clear(); return false; }
    posted_space.wait(lock, [&] { return posted_pending < Dispatcher::kPostedMax || failures->Failed() || stopped; });
    if (draw_batch.empty()) return true;  // Another flush took it meanwhile.
    auto batch = std::make_shared<std::vector<DrawRequest>>(std::move(draw_batch));
    draw_batch.clear();
    ++draw_batches;
    draws_batched += batch->size();
    batch_max = std::max<uint64_t>(batch_max, batch->size());
    switch (why) {
      case Flush::Full: ++batch_flush_full; break;
      case Flush::Operation: ++batch_flush_operation; break;
      case Flush::Run: ++batch_flush_run; break;
      case Flush::Close: ++batch_flush_close; break;
    }
    profile::Count(profile::kDrawBatches, 1);
    const bool ok = PostLocked(lock, [batch](Engine& engine, std::string& out) {
      engine.PrefetchPipelines(*batch);  // The absent pipelines of the batch, in parallel.
      for (const auto& request : *batch) {
        uint64_t fence = 0;
        if (!engine.DrawIndexed(request, fence, out)) return false;
      }
      return true;
    });
    return ok;
  }
};

namespace {
// The tasks that forward the title's calls through the stage.
class OperationTask final : public StageTask {
 public:
  OperationTask(Dispatcher& d, std::function<bool(Engine&, std::string&)> op) : d_(d), op_(std::move(op)) {}
  void Run() override { d_.Post(std::move(op_)); }
 private:
  Dispatcher& d_;
  std::function<bool(Engine&, std::string&)> op_;
};
class DrawRequestTask final : public StageTask {
 public:
  DrawRequestTask(Dispatcher& d, DrawRequest&& request) : d_(d), request_(std::move(request)) {}
  bool Discardable() const override { return true; }
  void Run() override { d_.PostDraw(std::move(request_)); }
 private:
  Dispatcher& d_;
  DrawRequest request_;
};
class RunTask final : public StageTask {
 public:
  RunTask(Dispatcher& d, std::function<bool(Engine&, std::string&)> op) : d_(d), op_(std::move(op)) {}
  ~RunTask() override {
    if (!done_) result_.set_value({false, "{\"reason\":\"dispatcher-closed\"}"});
  }
  std::future<std::pair<bool, std::string>> Future() { return result_.get_future(); }
  void Run() override {
    std::string evidence;
    const bool ok = d_.Run(std::move(op_), evidence);
    done_ = true;
    result_.set_value({ok, std::move(evidence)});
  }
 private:
  Dispatcher& d_;
  std::function<bool(Engine&, std::string&)> op_;
  std::promise<std::pair<bool, std::string>> result_;
  bool done_ = false;
};
class WorkTask final : public StageTask {
 public:
  explicit WorkTask(std::function<void()> work) : work_(std::move(work)) {}
  void Run() override { work_(); }
 private:
  std::function<void()> work_;
};
}  // namespace

Dispatcher::Dispatcher(bool dedicatedOwner, bool deferredTranslation)
    : state_(std::make_unique<State>(dedicatedOwner, dedicatedOwner && deferredTranslation)) {
  // A retained failure wakes the producers waiting for room: they return refused.
  State* const s = state_.get();
  s->failures->OnRetained([s] {
    std::lock_guard lock(s->mutex);
    s->posted_space.notify_all();
  });
}
Dispatcher::~Dispatcher() {
  std::string evidence;
  if (!Shutdown(evidence)) ExitUnreleased("dispatcher-destructor", evidence);
}

bool Dispatcher::Initialize(std::string& evidence, const RenderMode& mode) {
  std::lock_guard lifecycle(state_->lifecycle);
  auto result = std::make_shared<std::promise<std::pair<bool, std::string>>>();
  auto future = result->get_future();
  auto initialize = [this, result, mode] {
    state_->engine = std::make_unique<Engine>(state_->failures);
    std::string raw;
    bool ok = state_->engine->Initialize(raw, mode);
    result->set_value({ok, state_->dedicated ? fmt::format(
        "{{\"dedicated_owner\":true,\"owner_thread\":{},\"deferred_translation\":{},\"engine\":{}}}",
        rex::thread::current_thread_system_id(), state_->deferred, raw) : raw});
  };
  {
    std::lock_guard lock(state_->mutex);
    if (state_->initialized || state_->closing || state_->stopped) {
      evidence = "{\"reason\":\"dispatcher-duplicate-or-closed\"}";
      return false;
    }
    state_->initialized = true;
    if (state_->dedicated) { state_->Start(); state_->Queue(initialize); }
    if (state_->deferred) state_->StartStage();
  }
  if (!state_->dedicated) initialize();
  auto [ok, raw] = future.get();
  evidence = std::move(raw);
  return ok;
}
// The device is fixed from Initialize on; the promise orders its creation
// before this read.
std::string Dispatcher::DeviceName() const {
  std::lock_guard lifecycle(state_->lifecycle);
  return state_->engine ? state_->engine->DeviceName() : std::string();
}
GraphicsApi Dispatcher::Api() const {
  std::lock_guard lifecycle(state_->lifecycle);
  return state_->engine ? state_->engine->Api() : RenderMode{}.api;
}

bool Dispatcher::InitializeFrontend(uint64_t generation, std::string& evidence) {
  return Frontend(0, generation, evidence);
}
bool Dispatcher::VerifyFrontend(uint64_t generation, std::string& evidence) {
  return Frontend(1, generation, evidence);
}
bool Dispatcher::ReleaseFrontend(uint64_t generation, std::string& evidence) {
  return Frontend(2, generation, evidence);
}
bool Dispatcher::Frontend(unsigned operation, uint64_t generation, std::string& evidence) {
  auto result = std::make_shared<std::promise<std::pair<bool, std::string>>>();
  auto future = result->get_future();
  auto task = [this, result, operation, generation] {
    std::string raw;
    const bool ok = state_->engine && (operation == 0 ? state_->engine->InitializeFrontend(generation, raw) :
        operation == 1 ? state_->engine->VerifyFrontend(generation, raw) :
        operation == 2 ? state_->engine->ReleaseFrontend(generation, raw) : false);
    if (ok && operation == 2) {
      std::lock_guard lock(state_->mutex);
      state_->frontend_retained = false;
    }
    result->set_value({ok, raw.empty() ? "{\"reason\":\"frontend-owner-missing\"}" : raw});
  };
  {
    std::lock_guard lock(state_->mutex);
    if (!state_->initialized || state_->closing || state_->stopped) {
      evidence = "{\"reason\":\"frontend-dispatcher-closed\"}";
      return false;
    }
    if (operation == 0) state_->frontend_retained = true;
    if (state_->dedicated) state_->Queue(task);
  }
  if (!state_->dedicated) task();
  auto [ok, raw] = future.get();
  evidence = std::move(raw);
  return ok;
}

bool Dispatcher::Deferred() const { return state_->deferred; }
bool Dispatcher::OnTranslator() const {
  return state_->deferred && state_->translator_id.load(std::memory_order_relaxed) == rex::thread::current_thread_system_id();
}
bool Dispatcher::Enqueue(std::unique_ptr<StageTask> task) {
  auto& s = *state_;
  std::unique_lock lock(s.stage_mutex);
  if (!s.deferred || s.stage_stopped) return false;
  if (s.stage.size() >= kStageMax) {
    ++s.stage_waits;
    profile::Count(profile::kStageWaits, 1);
    ++s.stage_waiting;
    s.stage_space.wait(lock, [&] { return s.stage.size() < kStageMax || s.stage_stopped; });
    --s.stage_waiting;
    if (s.stage_stopped) return false;
  }
  const bool wake = s.translator_sleeping;
  s.stage.push_back(std::move(task));
  ++s.stage_enqueued;
  s.stage_max = std::max<uint64_t>(s.stage_max, s.stage.size());
  if (s.stage.size() > s.stage_depth_frame.load(std::memory_order_relaxed))
    s.stage_depth_frame.store(s.stage.size(), std::memory_order_relaxed);
  if (wake) {
    ++s.stage_notifications;
    lock.unlock();
    s.stage_wake.notify_one();
  }
  return true;
}
void Dispatcher::Defer(std::function<void()> work) {
  if (state_->deferred && !OnTranslator()) {
    if (Enqueue(std::make_unique<WorkTask>(std::move(work)))) return;
    return;  // Closed: the work is dropped with the stage.
  }
  work();
}
void Dispatcher::DiscardCaptures() { state_->discarding.store(true, std::memory_order_release); }
uint64_t Dispatcher::TakeStageDepth() { return state_->stage_depth_frame.exchange(0, std::memory_order_relaxed); }
std::string Dispatcher::StageSummary() {
  auto& s = *state_;
  std::lock_guard lock(s.stage_mutex);
  return fmt::format("\"deferred\":{},\"enqueued\":{},\"run\":{},\"discarded\":{},\"stops\":{},\"waits\":{},"
                     "\"max_depth\":{},\"pending\":{},\"stage_max\":{},\"wake_notifications\":{},"
                     "\"space_notifications\":{},\"consume_batches\":{},\"consume_batch_max\":{}", s.deferred, s.stage_enqueued, s.stage_run.load(),
                     s.stage_discarded.load(), s.stage_stops.load(), s.stage_waits, s.stage_max, s.stage.size(),
                     kStageMax, s.stage_notifications, s.stage_space_notifications, s.stage_consume_batches,
                     s.stage_consume_batch_max);
}

bool Dispatcher::Run(std::function<bool(Engine&, std::string&)> operation,
                                   std::string& evidence) {
  profile::Timer round_trip(profile::kDispatchRoundTrip);
  if (state_->deferred && !OnTranslator()) {  // Through the stage, after the captures before it.
    auto task = std::make_unique<RunTask>(*this, std::move(operation));
    auto future = task->Future();
    if (!Enqueue(std::move(task))) {
      evidence = "{\"reason\":\"dispatcher-closed\"}";
      return false;
    }
    auto [ok, raw] = future.get();
    evidence = std::move(raw);
    return ok;
  }
  auto result = std::make_shared<std::promise<std::pair<bool, std::string>>>();
  auto future = result->get_future();
  auto task = [this, result, operation = std::move(operation)] {
    std::string raw;
    profile::Timer engine_time(profile::kEngineOperation);
    const bool ok = state_->engine && operation(*state_->engine, raw);
    engine_time.Stop();
    result->set_value({ok, raw.empty() ? "{\"reason\":\"engine-missing\"}" : raw});
  };
  {
    std::unique_lock lock(state_->mutex);
    if (!state_->initialized || state_->closing || state_->stopped) {
      evidence = "{\"reason\":\"dispatcher-closed\"}";
      return false;
    }
    state_->FlushDrawsLocked(lock, State::Flush::Run);  // The draws recorded before this operation go first.
    if (state_->dedicated) state_->Queue(task);
  }
  if (!state_->dedicated) task();
  auto [ok, raw] = future.get();
  evidence = std::move(raw);
  return ok;
}

bool Dispatcher::Post(std::function<bool(Engine&, std::string&)> operation) {
  profile::Timer post_time(profile::kDispatchRoundTrip);  // Admission only.
  if (state_->deferred && !OnTranslator())  // Forwarded by the translator, in order with the captures.
    return Enqueue(std::make_unique<OperationTask>(*this, std::move(operation)));
  std::unique_lock lock(state_->mutex);
  state_->FlushDrawsLocked(lock, State::Flush::Operation);
  return state_->PostLocked(lock, std::move(operation));
}

bool Dispatcher::PostDraw(DrawRequest&& request) {
  if (state_->deferred && !OnTranslator())  // A ready request of the title, in order with the captures.
    return Enqueue(std::make_unique<DrawRequestTask>(*this, std::move(request)));
  std::unique_lock lock(state_->mutex);
  if (!state_->dedicated || !state_->initialized || state_->closing || state_->stopped || state_->failures->Failed())
    return false;
  if (state_->draw_batch.capacity() < kDrawBatchMax) state_->draw_batch.reserve(kDrawBatchMax);
  state_->draw_batch.push_back(std::move(request));
  if (state_->draw_batch.size() < kDrawBatchMax) return true;
  return state_->FlushDrawsLocked(lock, State::Flush::Full);
}

std::string Dispatcher::DrawBatchSummary() {
  std::lock_guard lock(state_->mutex);
  return fmt::format("\"batches\":{},\"draws\":{},\"batch_max\":{},\"open\":{},\"flush_full\":{},"
                     "\"flush_operation\":{},\"flush_run\":{},\"flush_close\":{},\"posted\":{}",
                     state_->draw_batches, state_->draws_batched, state_->batch_max, state_->draw_batch.size(),
                     state_->batch_flush_full, state_->batch_flush_operation, state_->batch_flush_run,
                     state_->batch_flush_close, state_->posted);
}

FailureChannel& Dispatcher::Failures() const { return *state_->failures; }

bool Dispatcher::PostedFailure(std::string& evidence) {
  const FailureChannel& failures = *state_->failures;
  if (!failures.Failed()) return false;
  const auto failure = failures.First();
  const uint64_t later = failures.Later();
  std::lock_guard lock(state_->mutex);
  // How many operations were posted after it, for a posted operation's failure.
  const std::string after = failure->source == FailureChannel::kPostedOperation ?
      std::to_string(state_->posted - failure->sequence) : std::string("null");
  evidence = fmt::format("{{\"posted_failed\":true,\"source\":\"{}\",\"failed_sequence\":{},\"posted\":{},"
                         "\"posted_after_failure\":{},\"dropped\":{},\"later_failures\":{},\"session\":{},"
                         "\"engine\":{}}}", failure->source, failure->sequence, state_->posted, after,
                         state_->posted_dropped, later, failures.Session(), failure->evidence);
  return true;
}

bool Dispatcher::Drain(std::string& evidence) {
  // FIFO: every operation posted before this WaitIdle has run (or was dropped)
  // when it returns, so a failure among them is already kept. Run goes
  // through the stage, so the captures before it are translated and posted first.
  const bool idle = Run([](Engine& engine, std::string& out) { return engine.WaitIdle(out); }, evidence);
  std::string failure;
  if (PostedFailure(failure)) {
    evidence = failure;
    return false;
  }
  return idle;
}

bool Dispatcher::StartPool(size_t count, unsigned threads,
                                         std::function<void(size_t, Engine&)> work,
                                         std::function<void(bool)> done) {
  std::lock_guard lifecycle(state_->lifecycle);  // The engine stays until Shutdown, which takes it too.
  {
    std::lock_guard lock(state_->mutex);
    if (!state_->dedicated || !state_->initialized || state_->closing || state_->stopped || !state_->engine ||
        !state_->pool.empty() || !count || !threads) return false;
  }
  Engine* engine = state_->engine.get();
  auto shared_work = std::make_shared<std::function<void(size_t, Engine&)>>(std::move(work));
  auto shared_done = std::make_shared<std::function<void(bool)>>(std::move(done));
  state_->pool_active = threads;
  for (unsigned t = 0; t < threads; ++t) {
    auto thread = StartPipelineThread([this, engine, count, t, shared_work, shared_done] {
      rex::thread::set_current_thread_name("Native Pipeline Compiler");
      // The pool yields to the game: while the title shows its loading screen
      // every thread compiles above normal priority (the player waits anyway);
      // in the title's menus every thread goes on at the lowest priority (no
      // world of the game runs there, and the menus keep their frames); with
      // the game running, only the first kPoolThreadsInGame threads go on,
      // at the lowest priority, and the others wait for the next loading screen
      // or menu (a cold driver cache takes some twenty seconds for the list on
      // ten threads, and the frames of the first seconds of a world paid for it).
      auto priority = platform::ThreadPriority::kNormal;
      const bool in_game_thread = t < kPoolThreadsInGame;
      for (;;) {
        // The wait comes before the claim: an entry claimed by a sleeping
        // thread would be nobody's (a draw would create it, the pool's done
        // callback would wait for the next loading screen) and the stop
        // would compile it before the join.
        while (!TitleLoading() && !TitleMenu() && !in_game_thread && !state_->pool_stop.load())
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (state_->pool_stop.load()) break;
        const size_t i = state_->pool_next.fetch_add(1);
        if (i >= count) break;
        const auto wanted = TitleLoading() ? platform::ThreadPriority::kAboveNormal : platform::ThreadPriority::kLowest;
        if (wanted != priority) platform::SetThreadPriority(priority = wanted);
        (*shared_work)(i, *engine);
      }
      if (state_->pool_active.fetch_sub(1) == 1 && *shared_done) (*shared_done)(state_->pool_stop.load());
    });
    if (thread) {
      state_->pool.push_back(std::move(thread));
      continue;
    }
    // A refused thread counts as finished; the others take its indexes.
    if (state_->pool_active.fetch_sub(1) == 1 && *shared_done) (*shared_done)(state_->pool_stop.load());
  }
  return true;
}

bool Dispatcher::WithEngine(const std::function<void(Engine&)>& operation) {
  // The engine stays until the operation returns. A Shutdown in progress holds
  // the lifecycle for its whole duration: the operation is optional, so the
  // caller (the title thread) is not made to wait for it.
  std::unique_lock lifecycle(state_->lifecycle, std::try_to_lock);
  if (!lifecycle.owns_lock()) return false;
  {
    std::lock_guard lock(state_->mutex);
    if (!state_->dedicated || !state_->initialized || state_->closing || state_->stopped || !state_->engine)
      return false;
  }
  operation(*state_->engine);
  return true;
}

bool Dispatcher::Shutdown(std::string& evidence) {
  narutorise::incident::Event("dispatcher-shutdown", "begin");
  std::lock_guard lifecycle(state_->lifecycle);
  // The stage stops first: its pending tasks are dropped (a pending Run
  // fails), the task in flight finishes, the translator is joined.
  if (state_->deferred) state_->StopStage();
  // The pool takes no new index and is joined before the engine goes.
  state_->pool_stop = true;
  for (auto& thread : state_->pool) JoinPipelineThread(*thread);
  auto result = std::make_shared<std::promise<std::pair<bool, std::string>>>();
  auto future = result->get_future();
  auto shutdown = [this, result] {
    std::string raw;
    bool ok = !state_->engine || state_->engine->Shutdown(raw);
    if (ok) state_->engine.reset(); // Destruction on the same owner.
    const auto owner = rex::thread::current_thread_system_id();
    result->set_value({ok, raw.empty() ? std::string{} : state_->dedicated ? fmt::format(
        "{{\"dedicated_owner\":true,\"owner_thread\":{},\"engine\":{}}}", owner, raw) : raw});
    if (ok && state_->dedicated) {
      std::lock_guard lock(state_->mutex);
      state_->stopped = true;
      state_->wake.notify_one();
      state_->posted_space.notify_all();  // A Post waiting for room returns refused.
    }
  };
  {
    std::unique_lock lock(state_->mutex);
    if (!state_->initialized || state_->stopped) {
      evidence.clear(); narutorise::incident::Event("dispatcher-shutdown", "end", "already-stopped"); return true;
    }
    state_->FlushDrawsLocked(lock, State::Flush::Close);  // Before the engine goes.
    state_->closing = true;
    if (state_->dedicated) state_->Queue(shutdown);
  }
  if (!state_->dedicated) shutdown();
  auto [ok, raw] = future.get();
  evidence = std::move(raw);
  if (ok) {
    // Real join of the owner thread: returns only after its original exit.
    if (state_->owner.joinable()) state_->owner.join();
    std::lock_guard lock(state_->mutex);
    state_->stopped = true;
  }
  // Failed draining leaves the engine and owner alive for retry or fatal stop.
  narutorise::incident::Event("dispatcher-shutdown", ok ? "end" : "failed", evidence);
  return ok;
}
}  // namespace gpu
