// Host test of the completion service of the Vulkan backend (vulkan/completion.h)
// with the dispatcher and the engine on a real device. Never enters a guest
// function. The test hooks hold the submissions on the GPU (a timeline
// semaphore they wait for), inject a result into the service's next wait and
// shorten its deadline.
// - Held: a frame lot whose submission waits on the GPU leaves its CloseFrame
//   event unset (the host publishes nothing) and a sampled copy released in
//   flight retired; once the hold is released, the event fires and the copy is
//   freed after the completion was observed.
// - An injected error, and the deadline, each after a CloseFrame that returned
//   success, with no operation posted after it: the failure and its native
//   result are in the channel when the event wakes the host, the publication
//   is refused, a producer waiting for room wakes refused, the operations
//   pending behind the failure are dropped and nothing more is submitted. The
//   hold is then released and Shutdown proves the completion.
// - Race: the owner closes frame lots in a loop while the service retains an
//   injected failure; nothing is submitted after the failure was retained.
// Usage: native_vulkan_completion_test <new output.jsonl>
#include "gpu/backend/backend.h"
#include "gpu/backend/dispatcher.h"
#include "gpu/backend/failure_channel.h"
#include <rex/thread.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <random>
#include <string>
#include <thread>

using namespace gpu;

namespace gpu::sampler {
void RegisterThread(const char*) {}  // The sampler is not linked here.
}  // namespace gpu::sampler

namespace {
constexpr uint32_t kSurface = 0x4400;
constexpr uint32_t kSampled = 0x6600;
constexpr int32_t kDeviceLost = -4;  // VK_ERROR_DEVICE_LOST.
bool Has(const std::string& evidence, const std::string& text) { return evidence.find(text) != std::string::npos; }
std::string Bool(bool value) { return value ? "true" : "false"; }
long long Count(const std::string& counts, const char* name) {
  const std::string key = std::string("\"") + name + "\":";
  const size_t at = counts.find(key);
  return at == std::string::npos ? -1 : std::strtoll(counts.c_str() + at + key.size(), nullptr, 10);
}
RenderMode Vulkan() {
  RenderMode mode;
  mode.api = GraphicsApi::kVulkan;
  return mode;
}

// A session: the dispatcher with its engine initialized on Vulkan, a surface
// and the completion hooks.
struct Session {
  Dispatcher dispatcher{true};
  CompletionTestHooks* hooks = nullptr;
  bool Open(std::string& evidence) {
    if (!dispatcher.Initialize(evidence, Vulkan())) return false;
    return dispatcher.Run([this](Engine& engine, std::string& out) {
      SurfaceDesc desc;
      desc.key = kSurface;
      desc.width = desc.height = 64;
      hooks = engine.CompletionHooksForTest();
      return hooks && engine.CreateSurface(desc, out);
    }, evidence);
  }
  bool Hold(std::string& evidence) {
    return dispatcher.Run([this](Engine&, std::string& out) { return hooks->HoldSubmissions(out); }, evidence);
  }
  // A clear of the surface, a sampled copy recorded in the same batch when
  // seed, then CloseFrame with the event.
  bool Lot(FrameEvent event, bool seed, std::string& evidence) {
    return dispatcher.Run([event, seed](Engine& engine, std::string& out) {
      ClearRequest clear;
      clear.color_keys[0] = kSurface;
      clear.clear_color = true;
      clear.color = {0, 0, 0, 1};
      uint64_t fence = 0, target = 0;
      if (!engine.ClearSurfaces(clear, fence, out)) return false;
      if (seed && !engine.SeedSampledForTest(kSampled, true, out)) return false;
      return engine.CloseFrame(event, target, out);
    }, evidence);
  }
  std::string Counts() {
    std::string counts;
    dispatcher.Run([](Engine& engine, std::string& out) {
      out = engine.ResourceCounts();
      return true;
    }, counts);
    return counts;
  }
};
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 || std::filesystem::exists(argv[1])) return 2;
  std::ofstream output(argv[1]);
  if (!output) return 3;
  bool all = true;
  auto line = [&](const std::string& kind, bool ok, const std::string& evidence) {
    all = all && ok;
    output << "{\"kind\":\"" << kind << "\",\"ok\":" << Bool(ok) << ",\"evidence\":"
           << (evidence.empty() ? "null" : evidence) << "}\n";
    output.flush();
  };
  std::string evidence;

  // Held: no event and no recycling until the submission completes.
  {
    Session s;
    const bool open = s.Open(evidence);
    line("held-open", open, evidence);
    if (!open) return 4;
    const FrameEvent event = CreateFrameEvent();
    const bool held = s.Hold(evidence);
    line("held-hold", held, evidence);
    const bool lot = s.Lot(event, true, evidence);
    line("held-lot-closed", lot && Has(evidence, "\"closed\":true"), evidence);
    const bool released = s.dispatcher.Run([](Engine& engine, std::string& out) {
      return engine.ReleaseSampled(kSampled, out);
    }, evidence);
    line("held-sampled-retired", released && Has(evidence, "\"deferred\":true"), evidence);
    const bool unset = rex::thread::Wait(event.get(), false, std::chrono::milliseconds(400)) ==
                       rex::thread::WaitResult::kTimeout;
    const std::string pending_counts = s.Counts();
    line("held-event-unset-copy-retained", unset && Count(pending_counts, "retired_sampled") == 1 &&
         Count(pending_counts, "sampled_released") == 0, pending_counts);
    const auto released_at = std::chrono::steady_clock::now();
    const bool release = s.hooks->ReleaseSubmissions(evidence);
    const bool fired = rex::thread::Wait(event.get(), false, std::chrono::milliseconds(5000)) == rex::thread::WaitResult::kSuccess;
    const double fired_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - released_at).count();
    line("held-released-event", release && fired, "{\"release\":" + evidence + ",\"fired_ms\":" +
         std::to_string(fired_ms) + "}");
    const bool idle = s.dispatcher.Run([](Engine& engine, std::string& out) { return engine.WaitIdle(out); }, evidence);
    const std::string counts = s.Counts();
    line("held-copy-freed-after-completion", idle && Count(counts, "retired_sampled") == 0 &&
         Count(counts, "sampled_released") == 1, "{\"idle\":" + evidence + ",\"counts\":" + counts + "}");
    std::string failure;
    line("held-no-failure", !s.dispatcher.PostedFailure(failure), failure.empty() ? "null" : failure);
    line("held-shutdown", s.dispatcher.Shutdown(evidence), evidence);
  }

  // A failure of the service after a CloseFrame that returned success.
  for (const char* mode : {"error", "deadline"}) {
    const std::string name = mode;
    Session s;
    const bool open = s.Open(evidence);
    line(name + "-open", open, evidence);
    if (!open) return 4;
    const FrameEvent event = CreateFrameEvent();
    const bool held = s.Hold(evidence);
    if (name == "deadline") s.hooks->SetDeadline(250);
    const bool lot = s.Lot(event, false, evidence);
    line(name + "-lot-closed", held && lot && Has(evidence, "\"closed\":true"), evidence);
    const uint64_t submitted = s.hooks->Submissions();
    // The owner is held by a gate while kPostedMax operations wait behind it;
    // one more producer then waits for room.
    std::promise<void> gate;
    std::shared_future<void> opened = gate.get_future().share();
    bool admitted = s.dispatcher.Post([opened](Engine&, std::string&) {
      opened.wait_for(std::chrono::seconds(10));
      return true;
    });
    for (size_t i = 1; i < Dispatcher::kPostedMax; ++i)
      admitted = admitted && s.dispatcher.Post([](Engine&, std::string& out) { out = "{}"; return true; });
    std::atomic<bool> producer_returned{false}, producer_admitted{true};
    std::thread producer([&] {
      producer_admitted = s.dispatcher.Post([](Engine&, std::string& out) { out = "{}"; return true; });
      producer_returned = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    line(name + "-producer-waits", admitted && !producer_returned.load(),
         "{\"admitted\":" + Bool(admitted) + "}");
    // The service finds the failure; nothing is posted after the lot.
    const auto started = std::chrono::steady_clock::now();
    if (name == "error") s.hooks->InjectResult(kDeviceLost);
    const bool woke = rex::thread::Wait(event.get(), false, std::chrono::milliseconds(5000)) == rex::thread::WaitResult::kSuccess;
    std::string failure;
    const bool visible = s.dispatcher.PostedFailure(failure);
    const double woke_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    const bool evidence_ok = Has(failure, "\"source\":\"vulkan-completion\"") &&
        (name == "error" ? Has(failure, "\"result\":-4") && Has(failure, "native-vulkan-completion-result")
                         : Has(failure, "\"result\":2") && Has(failure, "native-vulkan-completion-deadline"));
    line(name + "-failure-visible-at-wake", woke && visible && evidence_ok,
         "{\"woke_ms\":" + std::to_string(woke_ms) + ",\"failure\":" + (failure.empty() ? "null" : failure) + "}");
    const bool publication_refused = !s.dispatcher.Failures().Authorize([] {});
    for (int i = 0; i < 400 && !producer_returned.load(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const bool producer_refused = producer_returned.load() && !producer_admitted.load();
    const bool post_refused = !s.dispatcher.Post([](Engine&, std::string&) { return true; });
    line(name + "-publication-and-producers-refused", publication_refused && producer_refused && post_refused,
         "{\"publication_refused\":" + Bool(publication_refused) + ",\"producer_refused\":" +
             Bool(producer_refused) + ",\"post_refused\":" + Bool(post_refused) + "}");
    gate.set_value();
    std::string refused;
    const bool engine_refused = !s.dispatcher.Run([](Engine& engine, std::string& out) {
      ClearRequest clear;
      clear.color_keys[0] = kSurface;
      clear.clear_color = true;
      uint64_t fence = 0;
      return engine.ClearSurfaces(clear, fence, out);
    }, refused);
    s.dispatcher.PostedFailure(failure);
    const uint64_t submitted_after = s.hooks->Submissions();
    line(name + "-nothing-submitted-after", engine_refused && Has(refused, "vulkan-completion-failed") &&
         submitted_after == submitted && Has(failure, "\"dropped\":" + std::to_string(Dispatcher::kPostedMax - 1)),
         "{\"submitted_before\":" + std::to_string(submitted) + ",\"submitted_after\":" +
             std::to_string(submitted_after) + ",\"refused\":" + refused + ",\"failure\":" + failure + "}");
    producer.join();
    // The GPU completes once released; Shutdown proves it before releasing.
    const bool release = s.hooks->ReleaseSubmissions(evidence);
    const bool down = s.dispatcher.Shutdown(evidence);
    line(name + "-shutdown-after-completion", release && down && Has(evidence, "\"open_batch_discarded\":false"),
         evidence);
  }

  // Race between the service's failure and the owner's submissions.
  {
    constexpr int kRounds = 20;
    std::mt19937 random(11);
    int late = 0, failed_rounds = 0;
    uint64_t lots = 0;
    for (int round = 0; round < kRounds; ++round) {
      Session s;
      if (!s.Open(evidence)) {
        ++failed_rounds;
        continue;
      }
      std::atomic<uint64_t> at_failure{UINT64_MAX};
      s.dispatcher.Failures().OnRetained([&] { at_failure = s.hooks->Submissions(); });
      const FrameEvent event = CreateFrameEvent();
      std::atomic<bool> stop{false};
      std::atomic<uint64_t> closed{0};
      std::thread owner([&] {
        while (!stop.load()) {
          std::string out;
          if (!s.Lot(event, false, out)) break;
          ++closed;
        }
      });
      std::this_thread::sleep_for(std::chrono::microseconds(500 + random() % 20000));
      s.hooks->InjectResult(kDeviceLost);
      for (int i = 0; i < 400 && !s.dispatcher.Failures().Failed(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      stop = true;
      owner.join();
      lots += closed.load();
      if (!s.dispatcher.Failures().Failed() || at_failure.load() == UINT64_MAX) ++failed_rounds;
      else if (s.hooks->Submissions() != at_failure.load()) ++late;
      if (!s.dispatcher.Shutdown(evidence)) ++failed_rounds;
    }
    line("race", late == 0 && failed_rounds == 0,
         "{\"rounds\":" + std::to_string(kRounds) + ",\"lots\":" + std::to_string(lots) +
             ",\"submitted_after_failure\":" + std::to_string(late) + ",\"failed_rounds\":" +
             std::to_string(failed_rounds) + "}");
  }
  output << "{\"kind\":\"verdict\",\"passed\":" << Bool(all) << ",\"guest_executed\":false}\n";
  return all && output.good() ? 0 : 1;
}
