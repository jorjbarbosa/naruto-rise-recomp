// The native audio in the runtime (audio.h): the choice of backend, the
// guest side of the Output (its thread, the calls into the title, guest
// memory, the SDL device and the SDK's mix settings) and the native exports
// behind the gate (kernel_gate.h).
#include "audio/audio.h"
#include "audio/diagnostics.h"
#include "audio/kernel_gate.h"
#include "audio/measure.h"
#include "audio/output.h"
#include "audio/xma_runtime.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <fmt/format.h>
#include <rex/audio/flags.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#include <rex/thread.h>
#include <rex/types.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_STRING(audio_backend, "native", "narutorise/Audio",
                      "Audio output: native (the port's own) or sdk (the SDK's). Needs a restart.");

namespace narutorise::audio {
namespace {
using Clock = std::chrono::steady_clock;
// The status types the SDK's status macros name unqualified.
using rex::X_HRESULT;
using rex::X_RESULT;
using rex::X_STATUS;

std::atomic<bool> native{true};
std::atomic<bool> configured{false};
// After this many callbacks (about 5 s), the counters go to the log once.
constexpr uint64_t kRunningReport = 1000;
// While measuring (measure.h), its counts go to the log every 1875 callbacks (10 s).
constexpr uint64_t kMeasureReport = 1875;

// NARUTORISE_AUDIO_CAPTURE=<file.wav>: the samples put into the device, as a
// 32-bit float WAV, for the first NARUTORISE_AUDIO_CAPTURE_SECONDS (120). A
// diagnostic for gaps and level; it stops at a change of channel count.
class Capture {
 public:
  Capture() {
    if (const char* path = std::getenv("NARUTORISE_AUDIO_CAPTURE"); path && *path) path_ = path;
    const char* seconds = std::getenv("NARUTORISE_AUDIO_CAPTURE_SECONDS");
    limit_ = uint64_t(std::max(1, seconds && *seconds ? std::atoi(seconds) : 120)) * kSampleRate;
  }
  void Add(const float* samples, int channels) {
    if (path_.empty()) return;
    std::lock_guard lock(mutex_);
    if (done_) return;
    if (!file_) {
      file_ = std::fopen(path_.c_str(), "wb");
      if (!file_) {
        done_ = true;
        REXLOG_WARN("[narutorise] Audio capture: cannot write {}", path_);
        return;
      }
      channels_ = channels;
      Header(0);
      REXLOG_INFO("[narutorise] Audio capture: {} ({} ch, up to {} s)", path_, channels, limit_ / kSampleRate);
    }
    if (channels != channels_) {
      FinishLocked("the channel count changed");
      return;
    }
    std::fwrite(samples, sizeof(float), size_t(kFrameSamples) * channels, file_);
    frames_ += kFrameSamples;
    if (frames_ >= limit_) FinishLocked("complete");
  }
  void Finish() {
    std::lock_guard lock(mutex_);
    FinishLocked("closing");
  }

 private:
  void Header(uint64_t frames) {
    const uint32_t data = uint32_t(frames * channels_ * 4);
    const uint16_t format = 3, channels = uint16_t(channels_), bits = 32, align = uint16_t(channels_ * 4);
    const uint32_t rate = kSampleRate, bytes_per_second = kSampleRate * channels_ * 4, fmt_size = 16, riff = 36 + data;
    std::fseek(file_, 0, SEEK_SET);
    std::fwrite("RIFF", 1, 4, file_);
    std::fwrite(&riff, 4, 1, file_);
    std::fwrite("WAVEfmt ", 1, 8, file_);
    std::fwrite(&fmt_size, 4, 1, file_);
    std::fwrite(&format, 2, 1, file_);
    std::fwrite(&channels, 2, 1, file_);
    std::fwrite(&rate, 4, 1, file_);
    std::fwrite(&bytes_per_second, 4, 1, file_);
    std::fwrite(&align, 2, 1, file_);
    std::fwrite(&bits, 2, 1, file_);
    std::fwrite("data", 1, 4, file_);
    std::fwrite(&data, 4, 1, file_);
  }
  void FinishLocked(const char* reason) {
    if (!file_ || done_) {
      done_ = true;
      return;
    }
    Header(frames_);
    std::fclose(file_);
    file_ = nullptr;
    done_ = true;
    REXLOG_INFO("[narutorise] Audio capture {}: {:.1f} s in {}", reason, double(frames_) / kSampleRate, path_);
  }
  std::mutex mutex_;
  std::string path_;
  std::FILE* file_ = nullptr;
  bool done_ = false;
  int channels_ = 0;
  uint64_t frames_ = 0, limit_ = 0;
};

class KernelHost final : public OutputHost {
 public:
  bool StartThread(std::function<void()> body) override {
    auto* kernel = REX_KERNEL_STATE();
    if (!kernel) return false;
    kernel_ = kernel;
    thread_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(kernel, 128 * 1024, 0, [body = std::move(body)] {
          body();
          return 0;
        }));
    thread_->set_name("Native Audio");
    return thread_->Create() == X_STATUS_SUCCESS;
  }
  bool JoinThread(std::chrono::milliseconds timeout) override {
    auto* host = thread_ ? thread_->thread() : nullptr;
    if (!host) return true;
    return rex::thread::Wait(host, false, timeout) == rex::thread::WaitResult::kSuccess;
  }
  void CallClient(uint32_t callback, uint32_t argument) override {
    auto* state = rex::runtime::ThreadState::Get();
    if (!state || !kernel_) return;
    uint64_t args[] = {argument};
    kernel_->function_dispatcher()->Execute(state, callback, args, 1);
    const uint64_t calls = calls_.fetch_add(1) + 1;
    if (calls == kRunningReport) REXLOG_INFO("[narutorise] Audio running: {}", Summary());
    if (measure::Enabled() && calls % kMeasureReport == 0)
      REXLOG_INFO("[narutorise] Audio measure: {{\"seconds\":{},\"counts\":{}}}", calls / kMeasureReport * 10,
                  measure::Json());
  }
  const uint8_t* MapFrame(uint32_t address) override {
    auto* memory = Memory();
    if (!memory || !address || address > 0xFFFFFFFFu - (kFrameBytes - 1)) return nullptr;
    const uint32_t last = address + kFrameBytes - 1;
    auto* heap = memory->LookupHeap(address);
    if (!heap || heap != memory->LookupHeap(last)) return nullptr;
    const auto access = heap->QueryCommittedRangeAccess(address, last);
    if (!(static_cast<uint32_t>(access) & static_cast<uint32_t>(rex::memory::PageAccess::kReadOnly)))
      return nullptr;
    return memory->TranslateVirtual<const uint8_t*>(address);
  }
  std::unique_ptr<Device> OpenDevice(std::string& error) override { return OpenSdlDevice(error); }
  MixSettings Mix() override {
    MixSettings settings;
    settings.mute = REXCVAR_GET(audio_mute);  // The player's mute (narutorise.toml).
    return settings;
  }
  void Note(std::string_view event, std::string_view detail) override {
    if (event == "device-open-failed" || event == "device-lost" || event == "thread-start-failed" ||
        event == "register-refused" || event == "register-overflow")
      REXLOG_WARN("[narutorise] Audio {}: {}", event, detail);
    else
      REXLOG_INFO("[narutorise] Audio {}: {}", event, detail);
    if (event.starts_with("device-")) device_changed_.store(true);
  }
  void SleepUntil(Clock::time_point deadline) override {
    const auto remaining = deadline - Clock::now();
    if (remaining <= Clock::duration::zero()) return;
#ifdef _WIN32
    // Sleep rounds to the millisecond and to the system timer's period.
    thread_local HANDLE timer = [] {
      HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                             TIMER_ALL_ACCESS);
      return handle ? handle : CreateWaitableTimerW(nullptr, TRUE, nullptr);
    }();
    LARGE_INTEGER due;
    due.QuadPart = -std::max<LONGLONG>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100);
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(timer, INFINITE);
      return;
    }
#endif
    std::this_thread::sleep_until(deadline);
  }
  void Captured(const float* samples, int channels) override { capture_.Add(samples, channels); }
  void FinishCapture() { capture_.Finish(); }
  // True once after a device event: the crash report facts are refreshed.
  bool TakeDeviceChange() { return device_changed_.exchange(false); }

 private:
  rex::memory::Memory* Memory() {
    auto* kernel = kernel_ ? kernel_ : REX_KERNEL_STATE();
    return kernel ? kernel->memory() : nullptr;
  }
  rex::system::KernelState* kernel_ = nullptr;
  rex::system::object_ref<rex::system::XHostThread> thread_;
  std::atomic<uint64_t> calls_{0};
  std::atomic<bool> device_changed_{false};
  Capture capture_;
};

// Created by Configure with the native backend; lives until the process exits.
KernelHost* host = nullptr;
Output* output = nullptr;

void RefreshFacts() {
  if (host && output && host->TakeDeviceChange()) diagnostics::PublishDevice(BackendName(), output->Counters());
}

// The native exports. The title's arguments come as the SDK's exports take them.

u32 RegisterRenderDriverClient(mapped_u32 callback_ptr, mapped_u32 driver_ptr) {
  if (!output || !callback_ptr || !driver_ptr) return X_E_INVALIDARG;
  const uint32_t callback = callback_ptr[0];
  const uint32_t argument = callback_ptr[1];
  const uint32_t handle = output->Register(callback, argument);
  if (!handle) return X_E_INVALIDARG;
  *driver_ptr = handle;
  return X_ERROR_SUCCESS;
}

u32 UnregisterRenderDriverClient(mapped_void driver_ptr) {
  if (output) output->Unregister(driver_ptr.guest_address());
  return X_ERROR_SUCCESS;
}

u32 SubmitRenderDriverFrame(mapped_void driver_ptr, mapped_void samples_ptr) {
  if (output) {
    output->Submit(driver_ptr.guest_address(), samples_ptr.guest_address());
    RefreshFacts();
  }
  return X_ERROR_SUCCESS;
}

// No per-category volume on this port: the title asks for the change mask on
// every mix (sub_821502C8) and reads the volume of each of its 2 categories
// whose bit is set. Nothing ever changes, so the mask is 0; asked anyway, a
// category is at full volume.
u32 GetVoiceCategoryVolume(u32 category, mapped_f32 out_ptr) {
  (void)category;
  if (out_ptr) *out_ptr = 1.0f;
  return X_ERROR_SUCCESS;
}

u32 GetVoiceCategoryVolumeChangeMask(mapped_void driver_ptr, mapped_u32 out_ptr) {
  (void)driver_ptr;
  if (out_ptr) *out_ptr = 0;
  return X_ERROR_SUCCESS;
}
}  // namespace

void Configure() {
  std::string choice = REXCVAR_GET(audio_backend);
  std::string source = "setting";
  if (const char* environment = std::getenv("NARUTORISE_AUDIO_BACKEND"); environment && *environment) {
    choice = environment;
    source = "NARUTORISE_AUDIO_BACKEND";
  }
  bool use_native = true;
  if (choice == "sdk") {
    use_native = false;
  } else if (choice != "native") {
    REXLOG_WARN("[narutorise] Audio backend '{}' ({}) is not native or sdk; using native", choice, source);
  }
  native.store(use_native);
  configured.store(true);
  measure::Configure();
  if (use_native && !output) {
    host = new KernelHost();
    output = new Output(*host);
  }
  REXLOG_INFO("[narutorise] Audio backend: {} ({} '{}')", BackendName(), source, choice);
  diagnostics::PublishBackend(BackendName());
}

bool Native() { return native.load(std::memory_order_relaxed); }

void ConfigureRuntime(rex::RuntimeConfig& config) {
  if (!Native()) return;
  config.audio_factory = [](rex::runtime::FunctionDispatcher*) { return xma_runtime::CreateSystem(); };
}

const char* BackendName() { return Native() ? "native" : "sdk"; }

std::string Summary() {
  if (!output) return fmt::format("{{\"backend\":\"{}\"}}", BackendName());
  return diagnostics::Json(BackendName(), output->Counters());
}

void Closing() {
  if (output) output->MarkClosing();
}

void Shutdown() {
  if (measure::Enabled()) REXLOG_INFO("[narutorise] Audio measure at the close: {}", measure::Json());
  measure::FlushStreams();
  if (Native()) REXLOG_INFO("[narutorise] Audio XMA: {}", xma_runtime::Json());
  if (!output) return;
  output->Shutdown();
  host->FinishCapture();
  diagnostics::PublishDevice(BackendName(), output->Counters());
  REXLOG_INFO("[narutorise] Audio: {}", Summary());
}

namespace kernel {
template <>
void Call<0>(PPCContext& ctx, uint8_t* base) {
  rex::ppc::HostToGuestFunction<RegisterRenderDriverClient>(ctx, base);
}
template <>
void Call<1>(PPCContext& ctx, uint8_t* base) {
  rex::ppc::HostToGuestFunction<UnregisterRenderDriverClient>(ctx, base);
}
template <>
void Call<2>(PPCContext& ctx, uint8_t* base) {
  rex::ppc::HostToGuestFunction<SubmitRenderDriverFrame>(ctx, base);
}
template <>
void Call<3>(PPCContext& ctx, uint8_t* base) {
  rex::ppc::HostToGuestFunction<GetVoiceCategoryVolume>(ctx, base);
}
template <>
void Call<4>(PPCContext& ctx, uint8_t* base) {
  rex::ppc::HostToGuestFunction<GetVoiceCategoryVolumeChangeMask>(ctx, base);
}
}  // namespace kernel
}  // namespace narutorise::audio
