// The engines of the native mode: VdInitializeEngines on its one reviewed
// caller creates the own dispatcher and engine (no microcode, no callback).
#include "gpu/host/internal.h"
#include "gpu/backend/dispatcher.h"
#include "gpu/d3d/d3d.h"
#include "gpu/d3d/draw.h"
#include "gpu/common/guest_memory.h"
#include "gpu/settings.h"
#include <array>
#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>
#include <rex/system/xmemory.h>

namespace gpu::host {
std::mutex engine_mutex;
std::unique_ptr<Dispatcher> native_engine;
std::unique_ptr<Dispatcher> failed_native_engine;
EngineRegistration engine_registration{};

bool InitializeEngines(PPCContext& ctx) {
  auto* ts = rex::runtime::ThreadState::Get();
  auto* thread = rex::system::XThread::GetCurrentThread();
  // Coverage is one known caller/thread/CPU/signature, not the general Vd ABI.
  if (!ts || !thread || ts->thread_id() != 4 || thread->active_cpu() != 4 ||
      ctx.lr != 0x8213ED3C || ctx.r3.u32 != 0x18820000 || ctx.r4.u32 != 0x8213ECA0 ||
      ctx.r5.u32 != 0 || ctx.r6.u32 != 0x8206DCE0 || ctx.r7.u32 != 0x8206E160)
    Stop("VdInitializeEngines-outside-reviewed-signature", &ctx);
  constexpr std::array<uint8_t, 32> callback{{
      0x3d,0x60,0x7f,0xc8,0x39,0x40,0x00,0x07,0x91,0x4b,0x32,0x14,0x7c,0x00,0x06,0xac,
      0x39,0x40,0x08,0x00,0x91,0x4b,0x34,0x08,0x7c,0x00,0x06,0xac,0x4e,0x80,0x00,0x20}};
  std::array<uint8_t, 32> owned{};
  if (!CopyFromGuest(ts->memory(), ctx.r4.u32, owned.data(), owned.size()) || owned != callback)
    Stop("VdInitializeEngines-callback-variant", &ctx);
  // PFP/ME identify legacy hardware bootstrap, never native commands. Whole
  // lengths/semantics are unknown; package hashes are bound by the run preflight.
  // The exploratory capture is an observation only, not consumed by this engine.
  {
    std::lock_guard lock(event_mutex);
    Record("native-engine-init-before", "VdInitializeEngines-restricted-host-compatibility", &ctx,
           ctx.r31.u32);
  }
  std::string evidence, failed_release;
  bool failed_release_ok = true;
  bool ready = false, duplicate = false;
  {
    std::lock_guard lock(engine_mutex);
    duplicate = native_engine != nullptr;
    if (!duplicate) {
      gpu::AttachGuestMemory(ts->memory());  // For the threads without a guest context (the translator).
      auto candidate = std::make_unique<gpu::Dispatcher>(
          // Main and render threads both reach the D3D boundary.
          true, gpu::DeferredTranslation());  // The translation stage, by the session.
      const auto& settings = gpu::CurrentSettings();
      ready = candidate->Initialize(evidence, gpu::RenderMode{settings.resolution_scale, settings.msaa_samples,
          settings.graphics_device, settings.native_backend == "vulkan" ? GraphicsApi::kVulkan :
          settings.native_backend == "d3d12" ? GraphicsApi::kD3D12 : GraphicsApi(0xFF)});
      if (ready) {
        // The adapter in use, as the SDK logs it for the Xenos renderer.
        const std::string device = candidate->DeviceName();
        if (!settings.graphics_device.empty() && device != settings.graphics_device)
          REXLOG_WARN("[narutorise] graphics_device \"{}\" not found; the automatic choice is used",
                      settings.graphics_device);
        REXLOG_INFO("[narutorise] Native renderer backend: {}; adapter: {}; initialization: {}",
                    settings.native_backend, device, evidence);
        engine_registration = {ctx.r4.u32, ctx.r5.u32, ts->thread_id(), int(thread->active_cpu()), ctx.r31.u32};
        native_engine = std::move(candidate);
      } else {
        // The journal of the product is absent: the reason goes to the SDK log.
        REXLOG_ERROR("[narutorise] Native renderer initialization failed: {}", evidence);
        failed_release_ok = candidate->Shutdown(failed_release);
        if (!failed_release_ok) failed_native_engine = std::move(candidate);
      }
    }
  }
  if (duplicate) Stop("VdInitializeEngines-reinitialization-uncontracted", &ctx);
  {
    std::lock_guard lock(event_mutex);
    Write(fmt::format("{{\"sequence\":{},\"kind\":\"native-engine-init-result\","
        "\"ready\":{},\"evidence\":{},\"callback_invoked\":false,\"microcode_executed\":false}}\n",
        ++sequence, ready, evidence));
    if (!failed_release.empty()) {
      Write(fmt::format("{{\"sequence\":{},\"kind\":\"native-engine-failed-candidate-release\","
          "\"clean\":{},\"evidence\":{}}}\n", ++sequence, failed_release_ok, failed_release));
    }
  }
  if (!ready) Stop("VdInitializeEngines-native-init-failed-no-guest-return", &ctx, 0, 0, 88);
  // Same zero-extended u32 return as the PPC host ABI; original SDK stub is not
  // invoked. No guest memory, other register, callback or completion is changed.
  ctx.r3.u64 = 1;
  {
    std::lock_guard lock(event_mutex);
    Record("native-engine-init-after", "VdInitializeEngines-native-ready-return", &ctx);
    Modules();
  }
  // The own swapchain proof (diagnostics), before any title frame; it stops.
  if (gpu::Active() && gpu::PresentProofRequested()) gpu::PresentProof(ctx);
  return true;
}
Dispatcher* ActiveDispatcher() {
  std::lock_guard lock(engine_mutex);
  return native_engine.get();
}
}  // namespace gpu::host
