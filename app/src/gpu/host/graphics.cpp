// The graphics system of the native mode in place of the Xenos plugin: no
// presentation of its own, the GPU MMIO guarded, the watchdog, and the reviewed
// interrupt registration (owned, never dispatched).
#include "gpu/host/internal.h"
#include "gpu/common/guest_memory.h"
#include "gpu/host/interrupt_contract.h"
#include "gpu/backend/dispatcher.h"
#include "gpu/d3d/d3d.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <thread>
#include <fmt/format.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>
#include <rex/system/xmemory.h>

namespace gpu::host {
namespace {
// No own interrupt delivery reads the vblank status or writes the
// front-buffer register; every MMIO access stops.
uint32_t ReadMmio(void*, void*, uint32_t address) { Stop("gpu-mmio-read", Context(), address); }
void WriteMmio(void*, void*, uint32_t address, uint32_t value) {
  Stop("gpu-mmio-write-or-submission", Context(), address, value);
}

class GraphicsSystem final : public rex::system::IGraphicsSystem {
 public:
  rex::X_STATUS SetupPresentation(rex::ui::WindowedAppContext*) override {
    std::lock_guard lock(event_mutex);
    Record("host-setup", "presentation-absent", nullptr);
    Modules();
    return 0;
  }
  rex::X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* dispatcher,
                              rex::system::KernelState* kernel_state) override {
    dispatcher_ = dispatcher;
    kernel_state_ = kernel_state;
    if (!dispatcher->memory()->AddVirtualMappedRange(0x7FC80000, 0xFFF80000,
          0x00080000, this, ReadMmio, WriteMmio)) {
      Stop("mmio-guard-install-failed", nullptr, 0, 0, 88);
    }
    {
      std::lock_guard lock(event_mutex);
      Record("host-setup", "gpu-mmio-guard-installed-no-cp-no-vblank", nullptr);
      Modules();
    }
    // REX_NATIVE_WATCHDOG_SECONDS lengthens the window for runs that follow
    // the frame loop; the default stays 30 s in the diagnostic cases. "0", and
    // the game chosen by the renderer option, run without a watchdog: the boot
    // ends by the window close or a stop.
    int seconds = REXCVAR_GET(native_renderer_bootstrap) ? 30 : 0;
    if (const char* text = std::getenv("REX_NATIVE_WATCHDOG_SECONDS"); text && *text) {
      if (std::string_view(text) == "0") seconds = 0;
      else if (std::atoi(text) > 0) seconds = std::min(std::atoi(text), 600);
    }
    if (seconds) std::thread([seconds] {
      std::this_thread::sleep_for(std::chrono::seconds(seconds));
      const std::string name = seconds == 30 ? std::string("watchdog-30s-no-checkpoint") :
                               fmt::format("watchdog-{}s-no-checkpoint", seconds);
      // In the native mode the render thread takes the request at its next
      // own Swap or draw and stops in order (StopInOrder, engine released). If it
      // has not exited 10 s later, the direct stop runs; it parks if the ordered
      // stop already owns the exit.
      if (gpu::RequestStop(name, 87))
        for (int i = 0; i < 100; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      Stop(name, nullptr, 0, 0, 87);
    }).detach();
    return 0;
  }
  bool has_presentation() const override { return false; }
  rex::system::KernelState* kernel_state() const { return kernel_state_; }
  rex::runtime::FunctionDispatcher* dispatcher() const { return dispatcher_; }
  bool HasInitializerRegistration(uint32_t device) const {
    std::lock_guard lock(engine_mutex);
    return native_engine && interrupt_ && engine_registration.device == device &&
        engine_registration.guest_thread == 4 && engine_registration.cpu == 4 &&
        interrupt_->callback == 0x8212F950 && interrupt_->user_data == device &&
        interrupt_->guest_thread == 4 && interrupt_->cpu == 4 &&
        interrupt_->code == kInterruptCallbackCode && dispatcher_ && kernel_state_;
  }
  void ValidateInterruptRegistration(PPCContext& ctx) {
    auto* ts = rex::runtime::ThreadState::Get();
    auto* thread = rex::system::XThread::GetCurrentThread();
    // r4 is the device ("mr r4,r31" at 8213ED60; r31 is a local of 8213ECE8,
    // not in the context): the registration check below pins it.
    if (!ts || !thread || ts->thread_id() != 4 || thread->active_cpu() != 4 ||
        ctx.lr != 0x8213ED64 || ctx.r3.u32 != 0x8212F950)
      Stop("VdSetGraphicsInterruptCallback-outside-reviewed-signature", &ctx);
    bool valid = false;
    {
      std::lock_guard lock(engine_mutex);
      valid = native_engine && !interrupt_ && ctx.r4.u32 == engine_registration.device &&
              dispatcher_ && kernel_state_ && dispatcher_->memory() == ts->memory();
    }
    if (!valid) Stop("VdSetGraphicsInterruptCallback-owner-or-state-uncontracted", &ctx);
  }
  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    auto* ctx = Context();
    if (!ctx) Stop("SetInterruptCallback", ctx, callback, user_data);
    ValidateInterruptRegistration(*ctx);
    if (callback != ctx->r3.u32 || user_data != ctx->r4.u32)
      Stop("SetInterruptCallback-marshalled-argument-mismatch", ctx, callback, user_data);
    std::array<uint8_t, 184> owned{};
    auto* ts = rex::runtime::ThreadState::Get();
    if (!CopyFromGuest(ts->memory(), callback, owned.data(), owned.size()) || owned != kInterruptCallbackCode)
      Stop("VdSetGraphicsInterruptCallback-code-variant", ctx, callback, user_data);
    bool committed = false;
    {
      std::lock_guard lock(engine_mutex);
      if (native_engine && !interrupt_ && user_data == engine_registration.device) {
        interrupt_ = InterruptRegistration{callback, user_data, ts->thread_id(),
            int(rex::system::XThread::GetCurrentThread()->active_cpu()), owned};
        committed = true;
      }
    }
    if (!committed) Stop("SetInterruptCallback-owner-changed-before-commit", ctx, callback, user_data);
    const auto evidence = RegistrationEvidence();
    std::lock_guard lock(event_mutex);
    Record("native-interrupt-register-owned", "SDK-void-service-own-registration", ctx, callback, user_data);
    Write(fmt::format("{{\"sequence\":{},\"kind\":\"native-interrupt-registration\","
        "\"evidence\":{}}}\n", ++sequence, evidence));
  }
  std::string RegistrationEvidence() const {
    std::lock_guard lock(engine_mutex);
    if (!interrupt_) return {};
    std::string hex;
    for (auto byte : interrupt_->code) hex += fmt::format("{:02x}", byte);
    return fmt::format("{{\"callback\":{},\"user_data\":{},\"registered_guest_thread\":{},"
        "\"registered_cpu\":{},\"dispatcher_retained\":true,\"kernel_state_retained\":true,"
        "\"owned_code_bytes\":184,\"code_hex\":\"{}\",\"callback_invoked\":false,"
        "\"dispatch_implemented\":false,\"vblank_implemented\":false,"
        "\"async_scope\":\"registration-until-next-diagnostic-guard-only\"}}",
        interrupt_->callback, interrupt_->user_data, interrupt_->guest_thread, interrupt_->cpu, hex);
  }
  void InitializeRingBuffer(uint32_t ptr, uint32_t size) override {
    Stop("InitializeRingBuffer", Context(), ptr, size);
  }
  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t size) override {
    Stop("EnableReadPointerWriteBack", Context(), ptr, size);
  }
  void InitializeShaderStorage(const std::filesystem::path&, uint32_t, bool) override {
    std::lock_guard lock(event_mutex);
    Record("host-setup", "shader-storage-absent", nullptr);
  }
  void Shutdown() override {}
 private:
  struct InterruptRegistration {
    uint32_t callback, user_data, guest_thread;
    int cpu;
    std::array<uint8_t, 184> code;
  };
  std::optional<InterruptRegistration> interrupt_;
  rex::runtime::FunctionDispatcher* dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
};
GraphicsSystem* graphics_owner = nullptr;  // RuntimeConfig retains ownership.
}  // namespace

std::unique_ptr<rex::system::IGraphicsSystem> CreateGraphicsSystem() {
  auto graphics = std::make_unique<GraphicsSystem>();
  graphics_owner = graphics.get();
  return graphics;
}
std::string InterruptRegistrationEvidence() {
  return graphics_owner ? graphics_owner->RegistrationEvidence() : std::string{};
}
void ValidateInterruptRegistration(PPCContext& ctx) {
  if (!graphics_owner) Stop("VdSetGraphicsInterruptCallback-no-owner", &ctx);
  graphics_owner->ValidateInterruptRegistration(ctx);
}
rex::system::KernelState* Kernel() { return graphics_owner ? graphics_owner->kernel_state() : nullptr; }
bool InitializerRegistration(uint32_t device) {
  return graphics_owner && graphics_owner->HasInitializerRegistration(device);
}
}  // namespace gpu::host
