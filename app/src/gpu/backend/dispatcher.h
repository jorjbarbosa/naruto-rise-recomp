#pragma once

#include "gpu/backend/engine.h"
#include "gpu/backend/failure_channel.h"
#include <functional>
#include <memory>

namespace gpu {
// Host values only cross this boundary. Guest contexts and memory stay on
// their XThread; GPU operations and resource destruction stay on the owner.

// The translation stage. With
// the deferred translation on, everything the title sends to the engine enters
// one FIFO that the translator thread consumes in order: a draw capture is
// translated there (a task of the draw layer) and posted; Post, PostDraw, Run
// and Drain called from any other thread are forwarded by the translator, so an
// operation made after a draw reaches the owner after it, as before. Calls made
// on the translator itself go straight to the owner queue.
class StageTask {
 public:
  virtual ~StageTask() = default;
  virtual void Run() = 0;
  // A task the stage drops once a translation guard asked for the stop: the
  // draws after the culprit are not drawn; operations and runs still pass.
  virtual bool Discardable() const { return false; }
};
// Thrown out of a task by a guard of the translation after it asked the title
// to stop in order (RequestStop): the stage discards the discardable tasks
// from then on.
struct TranslationStopped {};

class Dispatcher final {
 public:
  explicit Dispatcher(bool dedicatedOwner, bool deferredTranslation = false);
  ~Dispatcher();
  bool Initialize(std::string& evidence, const RenderMode& mode = RenderMode{});  // The engine's render mode.
  std::string DeviceName() const;  // The engine's adapter, after Initialize.
  GraphicsApi Api() const;  // The engine's API, after Initialize.
  bool InitializeFrontend(uint64_t session_generation, std::string& evidence);
  bool VerifyFrontend(uint64_t session_generation, std::string& evidence);
  bool ReleaseFrontend(uint64_t session_generation, std::string& evidence);
  // Runs one engine operation on the owner thread and waits for its result.
  // Through the stage first when deferred (the operations before it run first).
  bool Run(std::function<bool(Engine&, std::string&)> operation, std::string& evidence);
  // Queues one engine operation on the owner thread without
  // waiting; the operation owns what it captures. FIFO with Run. The first
  // posted operation that fails is retained in the failure channel with its
  // evidence and every later posted operation is dropped unexecuted.
  // Admission waits while kPostedMax posted operations are pending, and is
  // refused once the channel holds a failure.
  static constexpr size_t kPostedMax = 512;
  bool Post(std::function<bool(Engine&, std::string&)> operation);
  // Draws recorded in a batch and posted in groups. PostDraw
  // appends the request to the open batch; the batch is posted as one
  // operation when it holds kDrawBatchMax draws or before any other operation
  // admitted here (Post, Run, Drain, Shutdown), so the FIFO with them holds.
  // The owner draws the batch in order and stops at its first failure, which
  // latches as a posted failure. Refused as Post is.
  static constexpr size_t kDrawBatchMax = 32;
  bool PostDraw(DrawRequest&& request);
  // Batches posted, draws they carried and why they closed (JSON fields).
  std::string DrawBatchSummary();
  // The session's failure channel (failure_channel.h), created with the
  // dispatcher and shared with its engine: the failure of a posted operation
  // and the ones the backend's services find. A retained failure refuses
  // every later admission and wakes the producers waiting for room. Nothing
  // may call Retain while holding a lock of the dispatcher.
  FailureChannel& Failures() const;
  // The failure the channel holds, if any, with the posted counters. Any
  // thread; the channel's flag answers without a lock when nothing failed.
  bool PostedFailure(std::string& evidence);
  // WaitIdle on the owner, refused when a posted operation failed before it:
  // the guard of every point that publishes a fence to the title.
  bool Drain(std::string& evidence);
  // Runs work(i, engine) for every i below count on `threads` pool threads,
  // concurrently with the owner; work may call only the engine's free-threaded
  // operations (PrecompilePipeline). The threads follow the title's loading
  // screen (common/title_loading.h): all of them at the highest priority while
  // it is up, kPoolThreadsInGame at the lowest otherwise. done(stopped) runs
  // once, on the last thread to finish. Shutdown stops the pool (no new index)
  // and joins it before the engine goes. One pool per dispatcher; false when
  // it ran already or the dispatcher is not running.
  static constexpr unsigned kPoolThreadsInGame = 2;
  bool StartPool(size_t count, unsigned threads, std::function<void(size_t, Engine&)> work,
                 std::function<void(bool)> done);
  // Runs operation(engine) on the calling thread, for the engine's
  // free-threaded operations only (PostPipeline, PrecompilePipeline); the
  // engine stays until it returns. False, without waiting, when the dispatcher
  // is not running or is shutting down.
  bool WithEngine(const std::function<void(Engine&)>& operation);
  // The translation stage. Deferred: the stage exists (fixed per session).
  bool Deferred() const;
  bool OnTranslator() const;  // The calling thread is the translator.
  // Queues a task of the stage; the caller waits while kStageMax tasks are
  // queued (the consumer may own a small local batch). False when the
  // stage is off or closed: the task is dropped. Not for the translator.
  static constexpr size_t kStageMax = 8192;
  bool Enqueue(std::unique_ptr<StageTask> task);
  // Runs work in order with the captures: on the stage when deferred and not
  // already on the translator, inline otherwise.
  void Defer(std::function<void()> work);
  // The stage drops its discardable tasks from now on (a stop asked outside a
  // task; inside one, TranslationStopped does it).
  void DiscardCaptures();
  std::string StageSummary();  // JSON fields.
  uint64_t TakeStageDepth();   // The deepest the queue got since the last take (the frame gauge).
  bool Shutdown(std::string& evidence);
 private:
  struct State;
  bool Frontend(unsigned operation, uint64_t session_generation, std::string& evidence);
  std::unique_ptr<State> state_;
};
}  // namespace gpu
