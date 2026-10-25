// Host test of the session's failure channel (failure_channel.h) with the
// dispatcher and the engine on a real device. Never enters a guest function.
// - Linearization: over many rounds of a racing retention, an action the
//   channel authorized never runs after the failure was retained, and nothing
//   is authorized once Retain returned.
// - Without a failure the consumers pass: a frame lot closes and completes,
//   the publication is authorized, Drain holds.
// - A failure retained by an asynchronous producer (standing for a backend's
//   completion service) after a frame lot closed with success, with no posted
//   operation after it: a producer waiting for room wakes refused, the
//   failure and its evidence reach PostedFailure intact, the publication and
//   later admissions are refused, the operations pending behind it are
//   dropped, the engine refuses with the same evidence, Drain reports it and
//   Shutdown releases.
// Usage: native_engine_failure_test <new output.jsonl>
#include "gpu/backend/dispatcher.h"
#include "gpu/backend/tests/test_backend.h"
#include "gpu/backend/failure_channel.h"
#include <rex/thread.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
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
bool Has(const std::string& evidence, const std::string& text) { return evidence.find(text) != std::string::npos; }
std::string Bool(bool value) { return value ? "true" : "false"; }
constexpr const char* kAsyncEvidence = "{\"native\":{\"api\":\"test\",\"result\":-4},\"fence\":4242}";

// A frame lot: a clear of the surface, then CloseFrame with its event; true
// when the event fired within the bound.
bool FrameLot(Dispatcher& d, uint32_t key, std::string& evidence) {
  const FrameEvent event = CreateFrameEvent();
  if (!event) return false;
  const bool closed = d.Run([key, event](Engine& engine, std::string& out) {
    ClearRequest clear;
    clear.color_keys[0] = key;
    clear.clear_color = true;
    clear.color = {0, 0, 0, 1};
    uint64_t fence = 0, target = 0;
    return engine.ClearSurfaces(clear, fence, out) && engine.CloseFrame(event, target, out);
  }, evidence);
  return closed && rex::thread::Wait(event.get(), false, std::chrono::milliseconds(5000)) == rex::thread::WaitResult::kSuccess;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 || std::filesystem::exists(argv[1])) return 2;
  // The debug layer is off by default (backend/d3d12); the host test asks for it.
  if (!std::getenv("REX_NATIVE_D3D12_DEBUG")) SetTestEnvironment("REX_NATIVE_D3D12_DEBUG", "1");
  std::ofstream output(argv[1]);
  if (!output) return 3;
  bool all = true;
  auto line = [&](const char* kind, bool ok, const std::string& evidence) {
    all = all && ok;
    output << "{\"kind\":\"" << kind << "\",\"ok\":" << Bool(ok) << ",\"evidence\":"
           << (evidence.empty() ? "null" : evidence) << "}\n";
    output.flush();
  };

  // Linearization: Retain races an authorizing loop; the loop sees, inside an
  // authorized action, whether Retain has already returned.
  {
    constexpr int kRounds = 2000;
    int late = 0, still_authorized = 0, wakes = 0;
    std::mt19937 random(7);
    for (int round = 0; round < kRounds; ++round) {
      FailureChannel channel;
      std::atomic<int> woken{0};
      channel.OnRetained([&] { woken.fetch_add(1); });
      std::atomic<bool> retained{false}, start{false};
      std::atomic<int> late_actions{0};
      std::thread authorizer([&] {
        while (!start.load()) {}
        while (channel.Authorize([&] { if (retained.load(std::memory_order_acquire)) late_actions.fetch_add(1); })) {}
      });
      const int spin = int(random() % 2000);
      start.store(true);
      for (volatile int i = 0; i < spin; ++i) {}
      channel.Retain({uint64_t(round), "race", "null"});
      retained.store(true, std::memory_order_release);
      authorizer.join();
      late += late_actions.load();
      still_authorized += channel.Authorize([] {}) ? 1 : 0;
      wakes += woken.load();
      if (channel.Retain({0, "later", "null"}) || channel.Later() != 1) ++still_authorized;
    }
    line("linearization", late == 0 && still_authorized == 0 && wakes == kRounds,
         "{\"rounds\":" + std::to_string(kRounds) + ",\"late_actions\":" + std::to_string(late) +
             ",\"authorized_after_retain\":" + std::to_string(still_authorized) + ",\"wakes\":" +
             std::to_string(wakes) + "}");
  }

  Dispatcher d(true);
  std::string evidence;
  const bool ready = d.Initialize(evidence, TestRenderMode());
  line("initialize", ready, evidence);
  if (!ready) {
    d.Shutdown(evidence);
    return 4;
  }
  constexpr uint32_t kKey = 0x4300;
  const bool surface = d.Run([](Engine& engine, std::string& out) {
    SurfaceDesc desc;
    desc.key = kKey;
    desc.width = desc.height = 64;
    return engine.CreateSurface(desc, out);
  }, evidence);
  line("surface", surface, evidence);

  // Without a failure.
  {
    const bool lot = FrameLot(d, kKey, evidence);
    std::string failure;
    const bool clean = !d.PostedFailure(failure) && !d.Failures().Failed();
    const bool authorized = d.Failures().Authorize([] {});
    std::string drained;
    const bool drain = d.Drain(drained);
    line("no-failure", lot && clean && authorized && drain,
         "{\"lot\":" + evidence + ",\"authorized\":" + Bool(authorized) + ",\"drain\":" + drained + "}");
  }

  // The asynchronous failure after a successful lot.
  const bool lot = FrameLot(d, kKey, evidence);
  line("lot-before-failure", lot, evidence);
  // The owner is held by a gate while kPostedMax operations wait behind it;
  // one more producer then waits for room.
  std::promise<void> gate;
  std::shared_future<void> opened = gate.get_future().share();
  bool admitted = d.Post([opened](Engine&, std::string&) {
    opened.wait_for(std::chrono::seconds(10));
    return true;
  });
  for (size_t i = 1; i < Dispatcher::kPostedMax; ++i)
    admitted = admitted && d.Post([](Engine&, std::string& out) { out = "{}"; return true; });
  std::atomic<bool> producer_returned{false}, producer_admitted{true};
  std::thread producer([&] {
    producer_admitted = d.Post([](Engine&, std::string& out) { out = "{}"; return true; });
    producer_returned = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const bool producer_blocked = !producer_returned.load();
  line("producer-waits-for-room", admitted && producer_blocked,
       "{\"admitted\":" + Bool(admitted) + ",\"blocked\":" + Bool(producer_blocked) + "}");
  // The asynchronous producer retains the failure; nothing is posted after it.
  const auto retained_at = std::chrono::steady_clock::now();
  bool first = false;
  std::thread([&] { first = d.Failures().Retain({4242, "completion-service-test", kAsyncEvidence}); }).join();
  for (int i = 0; i < 400 && !producer_returned.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  const auto woke_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - retained_at).count();
  const bool producer_refused = producer_returned.load() && !producer_admitted.load();
  line("producer-wakes-refused", first && producer_refused,
       "{\"first\":" + Bool(first) + ",\"returned\":" + Bool(producer_returned.load()) + ",\"admitted\":" +
           Bool(producer_admitted.load()) + ",\"woke_ms\":" + std::to_string(woke_ms) + "}");
  std::string failure;
  const bool visible = d.PostedFailure(failure) && Has(failure, "\"source\":\"completion-service-test\"") &&
                       Has(failure, "\"failed_sequence\":4242") && Has(failure, kAsyncEvidence) &&
                       Has(failure, "\"posted_after_failure\":null");
  line("failure-visible", visible, failure);
  const bool publication_refused = !d.Failures().Authorize([] {});
  const bool post_refused = !d.Post([](Engine&, std::string&) { return true; });
  DrawRequest draw;
  const bool draw_refused = !d.PostDraw(std::move(draw));
  line("publication-and-admission-refused", publication_refused && post_refused && draw_refused,
       "{\"publication_refused\":" + Bool(publication_refused) + ",\"post_refused\":" + Bool(post_refused) +
           ",\"draw_refused\":" + Bool(draw_refused) + "}");
  // The owner goes on: the operations pending behind the gate are dropped.
  gate.set_value();
  std::string refused;
  const bool engine_refused = !d.Run([](Engine& engine, std::string& out) {
    ClearRequest clear;
    clear.color_keys[0] = kKey;
    clear.clear_color = true;
    uint64_t fence = 0;
    return engine.ClearSurfaces(clear, fence, out);
  }, refused);
  line("engine-refuses", engine_refused && Has(refused, "\"reason\":\"completion-service-test-failed\"") &&
       Has(refused, "\"failed_sequence\":4242") && Has(refused, kAsyncEvidence), refused);
  d.PostedFailure(failure);
  const std::string dropped = "\"dropped\":" + std::to_string(Dispatcher::kPostedMax - 1);
  std::string drained;
  const bool drain_refused = !d.Drain(drained) && Has(drained, "\"failed_sequence\":4242");
  line("drain-refused", drain_refused && Has(failure, dropped),
       "{\"drain\":" + drained + ",\"failure\":" + failure + "}");
  producer.join();
  const bool down = d.Shutdown(evidence);
  line("shutdown", down && Has(evidence, "\"posted_failure_sequence\":4242"), evidence);
  output << "{\"kind\":\"verdict\",\"passed\":" << Bool(all) << ",\"guest_executed\":false}\n";
  return all && output.good() ? 0 : 1;
}
