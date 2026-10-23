// Measurement of the own frames (measure.h).
#include "gpu/common/platform.h"
#include "gpu/diagnostics/measure.h"
#include "gpu/diagnostics/diagnostics.h"
#include "gpu/diagnostics/frame_capture.h"
#include "gpu/diagnostics/start_evidence.h"
#include "gpu/d3d/hooks.h"
#include "gpu/d3d/device.h"
#include "gpu/d3d/resources.h"
#include "gpu/d3d/draw.h"
#include "gpu/d3d/frame.h"
#include "gpu/buffers/buffers.h"
#include "gpu/textures/textures.h"
#include "gpu/common/guest_memory.h"
#include "gpu/host/services.h"
#include "gpu/backend/dispatcher.h"
#include "gpu/diagnostics/profile.h"
#include "gpu/common/sha256.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <rex/thread.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>
#include <rex/system/xmemory.h>
#include <functional>
#include <thread>
#include <rex/runtime.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

using namespace gpu;

namespace gpu {
int64_t MeasureSwaps() {
  static const int64_t value = [] {
    if (!Diagnostics()) return int64_t(0);
    const char* text = std::getenv("REX_NATIVE_MEASURE_SWAPS");
    if (!text || !*text) return int64_t(0);
    const long n = std::atol(text);
    // Up to 100000 Swaps (about 28 min at 60), so a boot in the open world is
    // not stopped by the cap.
    return n >= 1 && n <= 100000 ? int64_t(n) : int64_t(-1);
  }();
  return value;
}

// A measurement file in REX_NATIVE_FRAME_DIR; returns its name and
// SHA-256 as JSON fields, or nulls without the directory.
std::string WriteFrameFile(const char* name, const std::string& text) {
  const char* dir = std::getenv("REX_NATIVE_FRAME_DIR");
  if (!dir || !*dir) return "\"file\":null,\"sha256\":null";
  std::FILE* out = std::fopen(fmt::format("{}/{}", dir, name).c_str(), "wb");
  const bool written = out && std::fwrite(text.data(), 1, text.size(), out) == text.size();
  if (out) std::fclose(out);
  if (!written) return fmt::format("\"file\":\"{}\",\"sha256\":\"write-failed\"", name);
  return fmt::format("\"file\":\"{}\",\"sha256\":\"{}\"", name, Sha256Hex(text.data(), text.size()));
}
// Guest and host threads of the kernel at the end of a measurement
// (the audio threads among them), with their host CPU times.
std::string ThreadCensus() {
  auto* current = rex::system::XThread::GetCurrentThread();
  auto* kernel = current ? current->kernel_state() : nullptr;
  if (!kernel) return "[]";
  std::string out;
  for (const auto& thread : kernel->object_table()->GetObjectsByType<rex::system::XThread>()) {
    auto* host = thread->thread();
    uint64_t kernel_time = 0, user_time = 0;
    const bool times = host && platform::ThreadTimes(host->native_handle(), kernel_time, user_time);
    std::string name;
    for (char c : host ? host->name() : std::string()) name += c == '"' || c == '\\' ? '_' : c;
    out += fmt::format("{}{{\"id\":{},\"guest\":{},\"running\":{},\"priority\":{},\"cpu\":{},\"host_tid\":{},"
                       "\"name\":\"{}\",\"kernel_100ns\":{},\"user_100ns\":{}}}", out.empty() ? "" : ",",
                       thread->thread_id(), thread->is_guest_thread(), thread->is_running(), thread->priority(),
                       unsigned(thread->active_cpu()), host ? host->system_id() : 0, name,
                       times ? kernel_time : 0, times ? user_time : 0);
  }
  return "[" + out + "]";
}
// Pacing samples, at each Swap entry once a red START candidate was
// drawn (both modes): host QPC; device +16532 (vblanks), +16536 (timebase at
// the last vblank), +16540 (vblank of the last flip), +16544 (Swaps), +16548
// (swap interrupts), +16552 (flips), +21676 (timebase at the last flip); and
// the START candidates of the frame that ended (first red c40.x word, red and
// white counts). Integers only, in pacing-samples.json.
struct PacingSample {
  int64_t qpc;
  std::array<uint32_t, 7> device;
  uint32_t red_word, red, white;
};
constexpr std::array<uint32_t, 7> kPacingDeviceWords{16532, 16536, 16540, 16544, 16548, 16552, 21676};
std::vector<PacingSample> pacing_samples;  // Swap thread only.
bool pacing_written = false;
std::string WritePacingSamples(bool own) {
  pacing_written = true;
  const int64_t frequency = platform::TickFrequency();
  std::string text = fmt::format("{{\"schema\":\"native-4-4b-pacing-samples-v1\",\"mode\":\"{}\",\"qpc_frequency\":{},"
                                 "\"requested\":{},\"columns\":\"qpc,vblank16532,timebase16536,last_flip16540,"
                                 "swaps16544,swap_interrupts16548,flips16552,timebase21676,red_word,red,white\","
                                 "\"samples\":[", own ? "own" : "reference", frequency, PacingSamples());
  for (size_t i = 0; i < pacing_samples.size(); ++i) {
    const auto& s = pacing_samples[i];
    text += fmt::format("{}\n[{},{},{},{},{}]", i ? "," : "", s.qpc, fmt::join(s.device, ","), s.red_word, s.red,
                        s.white);
  }
  text += "],\"threads\":" + ThreadCensus() + "}\n";
  return WriteFrameFile("pacing-samples.json", text);
}
void PacingAtSwap(PPCContext& ctx, uint8_t* base, uint32_t dev, bool own) {
  const uint64_t requested = PacingSamples();
  if (!requested) return;
  if (requested == UINT64_MAX) {
    if (own) StopInOrder(ctx, "pacing-samples-invalid", 88);
    std::_Exit(88);
  }
  const uint32_t red_word = pacing_red_word.exchange(0), red = pacing_red.exchange(0);
  const uint32_t white = pacing_white.exchange(0);
  if (!pacing_content.load() || pacing_written) return;
  if (pacing_samples.empty()) pacing_samples.reserve(size_t(requested));
  const int64_t now = platform::Ticks();
  PacingSample s{now, {}, red_word, red, white};
  for (size_t i = 0; i < kPacingDeviceWords.size(); ++i) s.device[i] = Get32(base, dev + kPacingDeviceWords[i]);
  pacing_samples.push_back(s);
  if (pacing_samples.size() < requested) return;
  const std::string file = WritePacingSamples(own);
  if (own) {
    host::Record("native-d3d-pacing-samples", file + fmt::format(",\"samples\":{}", pacing_samples.size()));
    return;  // The own measurement stops at MeasureSwaps.
  }
  std::fflush(nullptr);
  std::_Exit(86);  // Reference mode: the measurement ends with the file.
}

// The measurement mode (MeasureSwaps). At each own Swap entry the frame that
// just closed is sampled: the interval since the previous entry, own and START
// draws, and the profile slots, taken and reset. Frames are kept once START
// content has begun; at MeasureSwaps() frames they are written with integers
// only to REX_NATIVE_FRAME_DIR/measure-frames.json, summarized in the journal
// (the first 300 frames are warm-up), and the render thread stops in order.
struct MeasureFrame {
  uint64_t interval_ns = 0, draws = 0, start_draws = 0;
  std::array<uint64_t, profile::kCount> ns{}, calls{};
  std::array<uint64_t, profile::kCounterCount> counters{};
};
std::vector<MeasureFrame> measure_frames;  // Render thread only (own Swap).
constexpr size_t kMeasureWarmup = 300;
uint64_t Percentile(std::vector<uint64_t> values, size_t percent) {
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  const size_t rank = (values.size() * percent + 99) / 100;  // Nearest rank.
  return values[std::max<size_t>(rank, 1) - 1];
}
std::atomic<uint32_t> measure_thread{0};  // Host thread of MeasureAtSwap.
bool OnMeasureThread() {
  return measure_thread.load(std::memory_order_relaxed) == rex::thread::current_thread_system_id();
}
void MeasureAtSwap(PPCContext& ctx) {
  static uint64_t previous = 0, previous_draws = 0, previous_start = 0;
  measure_thread.store(rex::thread::current_thread_system_id(), std::memory_order_relaxed);
  const uint64_t now = profile::Now();
  MeasureFrame f;
  f.interval_ns = previous ? now - previous : 0;
  const uint64_t draws = own_draws.load(), start = start_draws.load();
  f.draws = draws - previous_draws;
  f.start_draws = start - previous_start;
  for (unsigned i = 0; i < profile::kCount; ++i) {
    f.ns[i] = profile::slots[i].ns.exchange(0, std::memory_order_relaxed);
    f.calls[i] = profile::slots[i].calls.exchange(0, std::memory_order_relaxed);
  }
  {  // The gauges of the frame, sampled at the Swap that closes it.
    const auto totals = TextureRegistrySnapshot();
    auto& c = profile::counters;
    c[profile::kSwapNumber].store(swaps.load() + 1, std::memory_order_relaxed);
    c[profile::kTextureBases].store(totals.bases, std::memory_order_relaxed);
    c[profile::kHeadersLive].store(resources.Live(), std::memory_order_relaxed);
    c[profile::kBuffersHosted].store(buffer_hosted_live.load(), std::memory_order_relaxed);
    auto* dispatcher = host::ActiveDispatcher();  // The stage depth of the frame.
    c[profile::kStageDepth].store(dispatcher ? dispatcher->TakeStageDepth() : 0, std::memory_order_relaxed);
  }
  for (unsigned i = 0; i < profile::kCounterCount; ++i)
    f.counters[i] = profile::counters[i].exchange(0, std::memory_order_relaxed);
  const bool content = previous && start;
  previous = now;
  previous_draws = draws;
  previous_start = start;
  if (!content) return;
  if (measure_frames.empty()) measure_frames.reserve(size_t(MeasureSwaps()));
  measure_frames.push_back(f);
  if (measure_frames.size() < size_t(MeasureSwaps())) return;
  WriteMeasureFrames(false);
  StopInOrder(ctx, "native-d3d-measure-complete", 86);
}
// The frames measured so far, written and summarized; complete at
// MeasureSwaps() frames (MeasureAtSwap), or partial at the watchdog stop and
// at the window close (TakeStopRequest), so a scene reached only by input
// (the Story Mode scene) has its profile. Render thread only.
void WriteMeasureFrames(bool partial) {
  static bool written_once = false;
  if (written_once || measure_frames.empty()) return;
  written_once = true;
  std::string text = "{\"schema\":\"native-4-4a-measure-frames-v1\",\"units\":\"ns\",\"slots\":[";
  for (unsigned i = 0; i < profile::kCount; ++i)
    text += fmt::format("{}\"{}\"", i ? "," : "", profile::kSlotNames[i]);
  text += "],\"counters\":[";
  for (unsigned i = 0; i < profile::kCounterCount; ++i)
    text += fmt::format("{}\"{}\"", i ? "," : "", profile::kCounterNames[i]);
  text += fmt::format("],\"partial\":{},\"columns\":\"interval_ns,draws,start_draws,slot ns...,slot calls...,counters...\","
                      "\"frames\":[", partial);
  for (size_t k = 0; k < measure_frames.size(); ++k) {
    const auto& m = measure_frames[k];
    text += fmt::format("{}[{},{},{},{},{},{}]", k ? ",\n" : "\n", m.interval_ns, m.draws, m.start_draws,
                        fmt::join(m.ns, ","), fmt::join(m.calls, ","), fmt::join(m.counters, ","));
  }
  text += "]}\n";
  std::string file = "null", file_sha = "null";
  if (const char* dir = std::getenv("REX_NATIVE_FRAME_DIR"); dir && *dir) {
    const std::string path = fmt::format("{}/measure-frames.json", dir);
    std::FILE* out = std::fopen(path.c_str(), "wb");
    const bool written = out && std::fwrite(text.data(), 1, text.size(), out) == text.size();
    if (out) std::fclose(out);
    file = written ? "\"measure-frames.json\"" : "\"write-failed\"";
    if (written) file_sha = "\"" + Sha256Hex(text.data(), text.size()) + "\"";
  }
  // The pacing records of the same run (own interrupt source and samples).
  const std::string pacing_file = WriteFrameFile("pacing.json", host::PacingJson() + "\n");
  const std::string samples_file = PacingSamples() && !pacing_written ? WritePacingSamples(true) : "null";
  const size_t warmup = measure_frames.size() > kMeasureWarmup ? kMeasureWarmup : 0;
  std::vector<uint64_t> intervals;
  uint64_t content_frames = 0, minimum_draws = UINT64_MAX;
  std::array<uint64_t, profile::kCount> sums{};
  std::array<uint64_t, profile::kCounterCount> counter_sums{};
  for (size_t k = warmup; k < measure_frames.size(); ++k) {
    const auto& m = measure_frames[k];
    intervals.push_back(m.interval_ns);
    content_frames += m.start_draws > 0;
    minimum_draws = std::min(minimum_draws, m.draws);
    for (unsigned i = 0; i < profile::kCount; ++i) sums[i] += m.ns[i];
    for (unsigned i = 0; i < profile::kCounterCount; ++i) counter_sums[i] += m.counters[i];
  }
  const uint64_t measured = intervals.size();
  std::string means;
  for (unsigned i = 0; i < profile::kCount; ++i)
    means += fmt::format("{}\"{}\":{}", i ? "," : "", profile::kSlotNames[i], measured ? sums[i] / measured / 1000 : 0);
  std::string counter_means;
  for (unsigned i = 0; i < profile::kCounterCount; ++i)
    counter_means += fmt::format("{}\"{}\":{}", i ? "," : "", profile::kCounterNames[i],
                                 measured ? counter_sums[i] / measured : 0);
  host::Record("native-d3d-measure", fmt::format(
      "\"partial\":{},\"frames\":{},\"warmup\":{},\"measured\":{},\"measured_with_start_draws\":{},\"minimum_draws\":{},"
      "\"interval_us\":{{\"median\":{},\"p95\":{},\"max\":{}}},\"mean_us_per_frame\":{{{}}},"
      "\"mean_count_per_frame\":{{{}}},\"file\":{},\"file_sha256\":{},\"swaps\":{},\"start_first_swap\":{},"
      "\"front_buffer_reads\":0,\"debug_layer_requested\":\"{}\",\"texture_integral_verify\":{},"
      "\"texture_uploads\":{},\"texture_binding_reuses\":{},\"pacing\":{{{}}},\"pacing_samples\":{},"
      "\"pacing_sample_count\":{},\"interrupts\":{},\"fences\":{},\"limiter\":{}", partial, measure_frames.size(), warmup, measured,
      content_frames, measured ? minimum_draws : 0, Percentile(intervals, 50) / 1000, Percentile(intervals, 95) / 1000,
      Percentile(intervals, 100) / 1000, means, counter_means, file, file_sha, swaps.load(), start_first_swap.load(),
      std::getenv("REX_NATIVE_D3D12_DEBUG") ? std::getenv("REX_NATIVE_D3D12_DEBUG") : "unset", TextureVerify(),
      texture_uploads.load(), texture_binding_reuses.load(), pacing_file,
      samples_file == "null" ? samples_file : "{" + samples_file + "}", pacing_samples.size(),
      host::InterruptSummary(), device_owner->FenceSummary(), LimiterSummary()));
  if (TransitionPeriod()) host::Record("native-d3d-transitions", TransitionSummary());
}
}  // namespace gpu
