// The pipelines of the draw layer between their creation and the owner's
// take: the set the pool threads of the embedded list fill (Precompile), the
// queue of the asynchronous threads (Post) and the takes of the draws (Take,
// TakeSkip). The engine keeps the pipelines a draw took (its own map) and
// creates the ones the set has none for; the set remembers which triple (VS,
// PS, layout) was compiled in this process (warm): the driver recreates a warm
// triple with another state in a few milliseconds instead of tens.
//
// Policy, as the UnleashedRecomp: a draw never skips. The owner posts the
// absent pipelines of a batch before drawing it (Engine::PrefetchPipelines);
// the draw takes its pipeline ready, waits for the thread creating it, or
// creates it itself. The asynchronous threads run at normal priority, and above
// it while the title shows its loading screen (common/title_loading.h).
// The skip take (REX_NATIVE_PSO_ASYNC=1, diagnostics) is the policy before
// 2026-10-17, kept for A/B: the draw of a pipeline not ready is recorded
// without itself, and the threads run at the lowest priority.
#pragma once

#include "gpu/common/platform.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "gpu/backend/pipeline_keys.h"
#include "gpu/backend/contracts.h"
#include <rex/thread.h>

namespace gpu {

// The threads that create pipelines: the pool of the embedded list
// (Dispatcher::StartPool) and the asynchronous threads of the set. A driver's
// shader compiler recurses deeply inside pipeline creation, so they get the
// stack of the SDK's own pipeline threads instead of the 1 MiB process
// default. Null when the system refused the thread.
std::unique_ptr<rex::thread::Thread> StartPipelineThread(std::function<void()> work);
void JoinPipelineThread(rex::thread::Thread& thread);
// The threads of each of those two pools. Both compile on a loading screen;
// together they leave two hardware threads to the title, the owner and the
// translator.
unsigned PipelineThreadsPerPool();

class PipelineSet {
 public:
  // A pipeline object of the backend, opaque here: the backend's creator makes
  // it, with the deleter of its API, and only the backend reads it.
  using Pipeline = std::shared_ptr<void>;
  // Creates the pipeline of a description, on any thread (the backend binds
  // its device and draw layout).
  using Creator = std::function<NativeResult(const PipelineDesc& desc, Pipeline& out)>;

  PipelineSet();
  ~PipelineSet();
  PipelineSet(const PipelineSet&) = delete;
  PipelineSet& operator=(const PipelineSet&) = delete;

  // Before the first Precompile, Post or Take (owner).
  void Configure(Creator creator);

  // One entry of the list, on a pool thread: creates key's pipeline into the
  // set unless the set has the key already (present: compiling, ready or the
  // owner's). The creation's result (success when present).
  NativeResult Precompile(uint64_t key, const PipelineDesc& desc, bool& present);

  // Posts key's creation to the asynchronous threads, started at the first
  // post (owner). only_if_absent: nothing when the set has the key (false).
  // lowest: the threads' base priority is the lowest (the skip take) instead
  // of normal. False after Stop.
  bool Post(uint64_t key, PipelineDesc desc, bool only_if_absent, bool lowest);

  // What a take gave the draw.
  struct Taken {
    Pipeline pipeline;
    // "precompiled" (the list), "async" (the threads) or "waited" (for either);
    // null: the draw creates the pipeline, or skips (skip).
    const char* origin = nullptr;
    bool skip = false;    // Skip take: being created; the draw records without itself.
    bool post = false;    // Skip take: the caller posts the description.
    bool stolen = false;  // Taken back from the asynchronous queue; the draw creates it.
    // The draw's own creation is a warm one, cheap: the triple was compiled in
    // this process (prefetch take), or the skip take allowed it inside the
    // frame's budget. The synchronous take never says so (origin "created").
    bool warm_creation = false;
    uint64_t wait_us = 0;  // "waited": for how long.
  };
  // The take of a draw (owner), by its policy. kSync and kPrefetch: a ready
  // pipeline is taken; one being created by a pool or an asynchronous thread
  // is waited for; one still queued is taken back (stolen); absent or failed,
  // the key becomes the owner's and the draw creates it. kSkip never waits:
  // ready, taken; being created, skip; absent, with a warm triple inside the
  // frame's budget (kWarmBudgetUs) the draw creates it, otherwise the key
  // enters the set as being created and the caller posts it (post), skip;
  // failed, the draw creates it.
  Taken Take(uint64_t key, uint64_t triple, DrawRequest::PipelineTake policy);
  // The draw created the pipeline of its take itself, in create_us: the
  // triple is warm from now on. Returns the origin for the record: "warm"
  // (warm_creation) or "created". A warm creation above kWarmSlowUs marks a
  // driver that does not keep the compiled shaders; in the skip take it turns
  // the warm creation off for the session.
  const char* NoteCreated(const Taken& take, uint64_t triple, uint32_t create_us);
  // The skip take's budget of warm creations is per frame.
  void EndFrame();
  // Stops and joins the asynchronous threads; what was queued fails (a take
  // then creates it). Clear releases the pipelines not taken and forgets the
  // warm triples, after the owner released its own.
  void Stop();
  void Clear();
  // The takes since the start, as JSON fields (any thread).
  std::string Summary() const;

 private:
  enum class Phase { Compiling, Ready, Failed, Owner };
  struct Entry {
    Phase phase = Phase::Compiling;
    Pipeline pipeline;
    bool async = false;   // Posted to the asynchronous threads.
    bool queued = false;  // Still in the queue (no thread took it).
  };
  // A warm creation is expected to take warm_estimate_us_; the budget of a
  // frame is kWarmBudgetUs; a creation above kWarmSlowUs is slow.
  static constexpr uint64_t kWarmBudgetUs = 3000, kWarmSlowUs = 8000;
  Taken TakeSync(uint64_t key, uint64_t triple, bool prefetch);
  Taken TakeSkip(uint64_t key, uint64_t triple);
  void Finish(uint64_t key, uint64_t triple, bool created, Pipeline pipeline);
  void StartThreads(bool lowest);  // Under mutex_.
  void Work(platform::ThreadPriority base_priority);

  Creator creator_;
  std::mutex mutex_;
  std::condition_variable done_;  // An entry left Compiling.
  std::condition_variable wake_;  // The queue or stop_ changed.
  std::unordered_map<uint64_t, Entry> entries_;
  std::deque<std::pair<uint64_t, PipelineDesc>> queue_;
  std::vector<std::unique_ptr<rex::thread::Thread>> threads_;
  bool stop_ = false;
  std::unordered_set<uint64_t> warm_triples_;
  // The skip take's warm creation (owner): the frame's spending, the estimate
  // of the next, and the option (REX_NATIVE_PSO_WARM_SYNC=0 turns it off).
  uint64_t warm_spent_us_ = 0, warm_estimate_us_ = 2500;
  bool warm_sync_;
  // The takes since the start (Summary).
  std::atomic<uint64_t> precompiled_{0}, async_created_{0}, adopted_{0}, waited_{0}, wait_us_{0}, wait_us_max_{0},
      stolen_{0}, created_{0}, warm_{0}, warm_slow_{0}, posted_{0}, skipped_{0};
};

}  // namespace gpu
