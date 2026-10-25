// The pipeline set of the draw layer (pipeline_set.h).
#include "gpu/backend/pipeline_set.h"
#include "gpu/common/shader_activity.h"

#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string_view>
#include <fmt/format.h>
#include "gpu/common/title_loading.h"
#include "gpu/diagnostics/profile.h"

namespace gpu {
namespace {
// REX_NATIVE_PSO_WARM_SYNC=0 keeps every absent pipeline of the skip take
// asynchronous, a warm triple included.
bool warmSyncOption() {
  static const bool value = [] {
    const char* text = std::getenv("REX_NATIVE_PSO_WARM_SYNC");
    return !(text && std::string_view(text) == "0");
  }();
  return value;
}
void StoreMax(std::atomic<uint64_t>& max, uint64_t value) {
  uint64_t seen = max.load(std::memory_order_relaxed);
  while (seen < value && !max.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {}
}
uint64_t ElapsedUs(std::chrono::steady_clock::time_point since) {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - since).count());
}
}  // namespace

PipelineSet::PipelineSet() : warm_sync_(warmSyncOption()) {}

PipelineSet::~PipelineSet() { Stop(); }

void PipelineSet::Configure(Creator creator) {
  creator_ = [creator = std::move(creator)](const PipelineDesc& desc, Pipeline& pipeline) {
    ShaderCompilation activity;
    return creator(desc, pipeline);
  };
}

NativeResult PipelineSet::Precompile(uint64_t key, const PipelineDesc& desc, bool& present) {
  present = false;
  {
    std::lock_guard lock(mutex_);
    if (!entries_.try_emplace(key).second) {
      present = true;
      return {};
    }
  }
  Pipeline pipeline;
  const NativeResult result = creator_(desc, pipeline);
  Finish(key, TripleKey(desc.vs.key, desc.ps.key, desc.layout_id), result.ok, std::move(pipeline));
  if (result.ok) precompiled_.fetch_add(1, std::memory_order_relaxed);
  return result;
}

// A creation ended (a pool or an asynchronous thread): the entry is ready
// or failed, the triple warm, and a waiting take wakes up.
void PipelineSet::Finish(uint64_t key, uint64_t triple, bool created, Pipeline pipeline) {
  {
    std::lock_guard lock(mutex_);
    Entry& entry = entries_[key];
    entry.phase = created ? Phase::Ready : Phase::Failed;
    entry.pipeline = std::move(pipeline);
    if (created) warm_triples_.insert(triple);
  }
  done_.notify_all();
}

bool PipelineSet::Post(uint64_t key, PipelineDesc desc, bool only_if_absent, bool lowest) {
  {
    std::lock_guard lock(mutex_);
    auto [it, inserted] = entries_.try_emplace(key);
    if (only_if_absent && !inserted) return false;
    if (stop_) {  // Nothing is created after Stop: the key fails, a take creates it.
      if (inserted) entries_.erase(it);
      else it->second.phase = Phase::Failed;
      return false;
    }
    it->second.async = it->second.queued = true;
    queue_.emplace_back(key, std::move(desc));
    if (threads_.empty()) StartThreads(lowest);
  }
  wake_.notify_one();
  posted_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// Two thirds of the hardware threads, at least two, as the recomps.
void PipelineSet::StartThreads(bool lowest) {
  const unsigned threads = std::max(2u, std::thread::hardware_concurrency() * 2 / 3);
  const int base = lowest ? THREAD_PRIORITY_LOWEST : THREAD_PRIORITY_NORMAL;
  for (unsigned i = 0; i < threads; ++i) threads_.emplace_back([this, base] { Work(base); });
}

void PipelineSet::Work(int base_priority) {
  SetThreadDescription(GetCurrentThread(), L"Native Pipeline Async");
  int priority = base_priority;
  SetThreadPriority(GetCurrentThread(), priority);
  for (;;) {
    std::pair<uint64_t, PipelineDesc> job;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (stop_) return;
      job = std::move(queue_.front());
      queue_.pop_front();
      entries_[job.first].queued = false;
    }
    // The title's loading screen waits for what compiles: the highest
    // priority then, the base otherwise (as the UnleashedRecomp's threads).
    const int wanted = TitleLoading() ? THREAD_PRIORITY_HIGHEST : base_priority;
    if (wanted != priority) {
      priority = wanted;
      SetThreadPriority(GetCurrentThread(), priority);
    }
    const PipelineDesc& d = job.second;
    Pipeline created;
    const NativeResult result = creator_(d, created);
    Finish(job.first, TripleKey(d.vs.key, d.ps.key, d.layout_id), result.ok, std::move(created));
    if (result.ok) async_created_.fetch_add(1, std::memory_order_relaxed);
  }
}

PipelineSet::Taken PipelineSet::Take(uint64_t key, uint64_t triple, DrawRequest::PipelineTake policy) {
  return policy == DrawRequest::PipelineTake::kSkip ? TakeSkip(key, triple)
                                                    : TakeSync(key, triple, policy == DrawRequest::PipelineTake::kPrefetch);
}

PipelineSet::Taken PipelineSet::TakeSync(uint64_t key, uint64_t triple, bool prefetch) {
  Taken take;
  std::unique_lock lock(mutex_);
  auto [it, inserted] = entries_.try_emplace(key);
  Entry& entry = it->second;  // A reference survives a rehash (node-based map); entries are never erased here.
  take.warm_creation = prefetch && warm_triples_.count(triple) != 0;
  if (inserted) {
    entry.phase = Phase::Owner;
    return take;
  }
  if (entry.queued) {
    const auto job = std::find_if(queue_.begin(), queue_.end(), [&](const auto& j) { return j.first == key; });
    if (job != queue_.end()) queue_.erase(job);
    entry.queued = false;
    entry.phase = Phase::Owner;
    take.stolen = true;
    stolen_.fetch_add(1, std::memory_order_relaxed);
    return take;
  }
  take.origin = entry.async ? "async" : "precompiled";
  if (entry.phase == Phase::Compiling) {
    take.origin = "waited";
    const auto started = std::chrono::steady_clock::now();
    done_.wait(lock, [&] { return entry.phase != Phase::Compiling; });
    take.wait_us = ElapsedUs(started);
    waited_.fetch_add(1, std::memory_order_relaxed);
    wait_us_.fetch_add(take.wait_us, std::memory_order_relaxed);
    StoreMax(wait_us_max_, take.wait_us);
    profile::Max(profile::kPipelineWaitUsMax, take.wait_us);
    profile::Count(profile::kPipelinesWaited, 1);
  }
  if (entry.phase == Phase::Ready) {
    take.pipeline = std::move(entry.pipeline);
    adopted_.fetch_add(1, std::memory_order_relaxed);
    if (take.wait_us == 0) profile::Count(profile::kPipelinesAdopted, 1);
  } else {
    take.origin = nullptr;  // Failed: the draw creates it.
  }
  entry.phase = Phase::Owner;
  return take;
}

PipelineSet::Taken PipelineSet::TakeSkip(uint64_t key, uint64_t triple) {
  Taken take;
  std::lock_guard lock(mutex_);
  auto [it, inserted] = entries_.try_emplace(key);
  Entry& entry = it->second;
  if (inserted && warm_sync_ && warm_triples_.count(triple) &&
      (!warm_spent_us_ || warm_spent_us_ + warm_estimate_us_ <= kWarmBudgetUs)) {
    entry.phase = Phase::Owner;
    take.warm_creation = true;
    return take;
  }
  if (inserted || entry.phase == Phase::Compiling) {
    if (inserted) {
      entry.async = true;
      take.post = true;
      profile::Count(profile::kPipelinesAsync, 1);
    }
    take.skip = true;
    skipped_.fetch_add(1, std::memory_order_relaxed);
    profile::Count(profile::kPipelineSkippedDraws, 1);
    return take;
  }
  if (entry.phase == Phase::Ready) {
    take.pipeline = std::move(entry.pipeline);
    take.origin = entry.async ? "async" : "precompiled";
    adopted_.fetch_add(1, std::memory_order_relaxed);
    profile::Count(profile::kPipelinesAdopted, 1);
  }
  entry.phase = Phase::Owner;
  return take;
}

const char* PipelineSet::NoteCreated(const Taken& take, uint64_t triple, uint32_t create_us) {
  NoteShaderCompiled();
  {
    std::lock_guard lock(mutex_);
    warm_triples_.insert(triple);
  }
  if (!take.warm_creation) {
    created_.fetch_add(1, std::memory_order_relaxed);
    profile::Count(profile::kPipelinesCreated, 1);
    return "created";
  }
  // The skip take's budget and estimate (owner); a slow driver turns its warm
  // creation off. The prefetch take comes here too (its warm creations are the
  // same kind); nothing reads the budget in that policy.
  warm_spent_us_ += create_us;
  warm_estimate_us_ = (3 * warm_estimate_us_ + create_us) / 4;
  if (create_us > kWarmSlowUs) warm_sync_ = false;
  warm_.fetch_add(1, std::memory_order_relaxed);
  profile::Count(profile::kPipelinesWarm, 1);
  if (create_us > kWarmSlowUs) {
    warm_slow_.fetch_add(1, std::memory_order_relaxed);
    profile::Count(profile::kPipelinesWarmSlow, 1);
  }
  return "warm";
}

void PipelineSet::EndFrame() { warm_spent_us_ = 0; }

void PipelineSet::Stop() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
    for (const auto& [key, desc] : queue_) {
      Entry& entry = entries_[key];
      entry.queued = false;
      entry.phase = Phase::Failed;
    }
    queue_.clear();
  }
  wake_.notify_all();
  done_.notify_all();
  for (auto& thread : threads_) thread.join();
  threads_.clear();
}

void PipelineSet::Clear() {
  std::lock_guard lock(mutex_);
  entries_.clear();
  warm_triples_.clear();
  warm_spent_us_ = 0;
  stop_ = false;  // The threads were joined by Stop; a later Post starts them again.
}

std::string PipelineSet::Summary() const {
  auto n = [](const std::atomic<uint64_t>& v) { return v.load(std::memory_order_relaxed); };
  return fmt::format("\"precompiled\":{},\"async_created\":{},\"adopted\":{},\"waited\":{},\"wait_us\":{},"
                     "\"wait_us_max\":{},\"stolen\":{},\"created\":{},\"warm\":{},\"warm_slow\":{},\"posted\":{},"
                     "\"skipped_draws\":{}",
                     n(precompiled_), n(async_created_), n(adopted_), n(waited_), n(wait_us_), n(wait_us_max_),
                     n(stolen_), n(created_), n(warm_), n(warm_slow_), n(posted_), n(skipped_));
}

}  // namespace gpu
