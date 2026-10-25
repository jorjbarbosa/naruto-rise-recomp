// The engine (engine.h): the facade of the host graphics API. Initialize
// selects the backend of RenderMode::api (backend.h) and every operation runs
// on it. Before a backend exists an operation is refused and the summaries
// are empty. Once the session's failure channel holds a failure, the GPU
// operations are refused here, for every backend alike.
#include "gpu/backend/engine.h"
#include "gpu/backend/backend.h"
#include "gpu/backend/failure_channel.h"

#include <cmath>

#include <fmt/format.h>
#include <fmt/ranges.h>

namespace gpu {
namespace {
bool NoBackend(std::string& evidence) {
  evidence = "{\"reason\":\"native-backend-not-initialized\"}";
  return false;
}
const char* ApiName(GraphicsApi api) {
  switch (api) {
    case GraphicsApi::kD3D12: return "d3d12";
    case GraphicsApi::kVulkan: return "vulkan";
  }
  return "unknown";
}
}  // namespace

Engine::Engine() : Engine(std::make_shared<FailureChannel>()) {}
Engine::Engine(std::shared_ptr<FailureChannel> failures) : failures_(std::move(failures)) {}
// The backend's destructor refuses to release work whose completion is unproved.
Engine::~Engine() = default;

bool Engine::Refused(std::string& evidence) const {
  if (!failures_->Failed()) return false;
  evidence = failures_->Refusal();
  return true;
}

bool Engine::Initialize(std::string& evidence, const RenderMode& mode) {
  if (!backend_) {
    api_ = mode.api;
#if !defined(NARUTORISE_NATIVE_D3D12_DISABLED)
    if (mode.api == GraphicsApi::kD3D12) backend_ = CreateD3d12Backend(failures_);
#endif
#if defined(NARUTORISE_NATIVE_VULKAN)
    if (mode.api == GraphicsApi::kVulkan) backend_ = CreateVulkanBackend(failures_);
#endif
    if (!backend_) {
      evidence = fmt::format("{{\"operation\":\"native-backend-unavailable\",\"api\":\"{}\","
                             "\"readiness_criteria_met\":false}}", ApiName(mode.api));
      return false;
    }
  }
  return backend_->Initialize(evidence, mode);
}
std::string Engine::DeviceName() const { return backend_ ? backend_->DeviceName() : std::string(); }
bool Engine::Shutdown(std::string& evidence) {
  if (!backend_) {
    evidence = "{\"released\":true,\"backend\":null}";
    return true;
  }
  return backend_->Shutdown(evidence);
}
bool Engine::InitializeFrontend(uint64_t session_generation, std::string& evidence) {
  return backend_ ? backend_->InitializeFrontend(session_generation, evidence) : NoBackend(evidence);
}
bool Engine::VerifyFrontend(uint64_t session_generation, std::string& evidence) {
  return backend_ ? backend_->VerifyFrontend(session_generation, evidence) : NoBackend(evidence);
}
bool Engine::ReleaseFrontend(uint64_t session_generation, std::string& evidence) {
  return backend_ ? backend_->ReleaseFrontend(session_generation, evidence) : NoBackend(evidence);
}
bool Engine::CreateSurface(const SurfaceDesc& desc, std::string& evidence) {
  if (Refused(evidence)) return false;
  return backend_ ? backend_->CreateSurface(desc, evidence) : NoBackend(evidence);
}
bool Engine::DestroySurface(uint32_t key, std::string& evidence) {
  if (Refused(evidence)) return false;
  return backend_ ? backend_->DestroySurface(key, evidence) : NoBackend(evidence);
}
bool Engine::EnsureSurface(const SurfaceDesc& desc, std::string& evidence) {
  if (Refused(evidence)) return false;
  return backend_ ? backend_->EnsureSurface(desc, evidence) : NoBackend(evidence);
}
bool Engine::ClearSurfaces(const ClearRequest& request, uint64_t& fence, std::string& evidence) {
  if (Refused(evidence)) return false;
  return backend_ ? backend_->ClearSurfaces(request, fence, evidence) : NoBackend(evidence);
}
bool Engine::WaitIdle(std::string& evidence) {
  if (!backend_) return NoBackend(evidence);
  if (!failures_->Failed()) return backend_->WaitIdle(evidence);
  // After a failure the open batch is never submitted; the batches submitted
  // before are still waited for, and the wait reports false.
  std::string idle;
  backend_->WaitSubmitted(idle);
  evidence = failures_->Refusal();
  evidence.insert(evidence.size() - 1, ",\"submitted_work_idle\":" + idle);
  return false;
}
bool Engine::CloseFrame(void* event, uint64_t& target, std::string& evidence) {
  if (Refused(evidence)) return false;
  return backend_ ? backend_->CloseFrame(event, target, evidence) : NoBackend(evidence);
}
bool Engine::AttachWindow(void* window, std::string& evidence, const PresentMode& mode) {
  if (Refused(evidence)) return false;
  return backend_ ? backend_->AttachWindow(window, evidence, mode) : NoBackend(evidence);
}
bool Engine::Reconfigure(const RenderMode& render, const PresentMode& present, std::string& evidence) {
  if (Refused(evidence)) return false;
  return backend_ ? backend_->Reconfigure(render, present, evidence) : NoBackend(evidence);
}
bool Engine::DetachWindow(std::string& evidence) {
  return backend_ ? backend_->DetachWindow(evidence) : NoBackend(evidence);
}
bool Engine::SetGammaRamp(const std::array<uint16_t, 768>& words, std::string& evidence) {
  if (Refused(evidence)) return false;
  return backend_ ? backend_->SetGammaRamp(words, evidence) : NoBackend(evidence);
}
bool Engine::SetVsync(bool enabled, std::string& evidence) {
  return backend_ ? backend_->SetVsync(enabled, evidence) : NoBackend(evidence);
}
bool Engine::StageFront(uint32_t key, uint32_t& slot, std::string& evidence) {
  if (Refused(evidence)) return false;
  return backend_ ? backend_->StageFront(key, slot, evidence) : NoBackend(evidence);
}
bool Engine::Present(uint32_t slot, bool capture, std::vector<uint8_t>& bgra, uint32_t& width, uint32_t& height,
                     std::string& evidence, bool* presented) {
  if (presented) *presented = false;
  if (Refused(evidence)) return false;
  bool shown = false;
  const bool ok = backend_ ? backend_->Present(slot, capture, bgra, width, height, evidence, shown) : NoBackend(evidence);
  if (presented) *presented = ok && shown;
  return ok;
}
// The descriptions of the UI layer are checked here, once for both backends.
bool Engine::UploadUiTexture(uint32_t key, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba,
                             std::string& evidence) {
  if (Refused(evidence)) return false;
  if (!key || !width || !height || width > 8192 || height > 8192 || rgba.size() != size_t(width) * height * 4) {
    evidence = fmt::format("{{\"uploaded\":false,\"reason\":\"native-ui-texture-description\",\"key\":{},"
                           "\"size\":[{},{}],\"bytes\":{}}}", key, width, height, rgba.size());
    return false;
  }
  return backend_ ? backend_->UploadUiTexture(key, width, height, rgba, evidence) : NoBackend(evidence);
}
bool Engine::ReleaseUiTexture(uint32_t key, std::string& evidence) {
  return backend_ ? backend_->ReleaseUiTexture(key, evidence) : NoBackend(evidence);
}
bool Engine::SetUiFrame(UiFrame frame, std::string& evidence) {
  const auto refuse = [&](const char* reason, size_t draw) {
    evidence = fmt::format("{{\"set\":false,\"reason\":\"{}\",\"draw\":{}}}", reason, draw);
    return false;
  };
  if (!std::isfinite(frame.width) || !std::isfinite(frame.height) || frame.width < 0 || frame.height < 0)
    return refuse("native-ui-frame-space", 0);
  for (size_t i = 0; i < frame.draws.size(); ++i) {
    const UiDraw& d = frame.draws[i];
    if (d.scissor && !(std::isfinite(d.scissor_left) && std::isfinite(d.scissor_top) &&
                       std::isfinite(d.scissor_right) && std::isfinite(d.scissor_bottom)))
      return refuse("native-ui-frame-scissor", i);
    if (d.index_offset > frame.indices.size() || d.index_count > frame.indices.size() - d.index_offset)
      return refuse("native-ui-frame-indices", i);
    for (uint32_t k = 0; k < d.index_count; ++k)
      if (uint64_t(d.vertex_offset) + frame.indices[d.index_offset + k] >= frame.vertices.size())
        return refuse("native-ui-frame-vertices", i);
  }
  return backend_ ? backend_->SetUiFrame(std::move(frame), evidence) : NoBackend(evidence);
}
std::string Engine::ResourceCounts() { return backend_ ? backend_->ResourceCounts() : std::string("{}"); }
bool Engine::ReleaseTexture(uint32_t key, std::string& evidence) {
  return backend_ ? backend_->ReleaseTexture(key, evidence) : NoBackend(evidence);
}
bool Engine::ReleaseSampled(uint32_t address, std::string& evidence) {
  return backend_ ? backend_->ReleaseSampled(address, evidence) : NoBackend(evidence);
}
// The backend's own releases, then its WaitIdle: a failure retained before
// refuses the operations of the facade, not these.
bool Engine::ReleaseRemaining(std::string& evidence) {
  if (!backend_) return NoBackend(evidence);
  ResourceKeys keys;
  if (!backend_->LiveResources(keys, evidence)) return false;
  std::string ignored;
  for (uint32_t key : keys.surfaces)
    if (!backend_->DestroySurface(key, ignored)) {
      evidence = fmt::format("{{\"released\":false,\"surface\":{},\"engine\":{}}}", key, ignored);
      return false;
    }
  for (uint32_t key : keys.textures)
    if (!backend_->ReleaseTexture(key, ignored)) {
      evidence = fmt::format("{{\"released\":false,\"texture\":{},\"engine\":{}}}", key, ignored);
      return false;
    }
  // The sampled copies whose header the title did not destroy.
  for (uint32_t address : keys.sampled)
    if (!backend_->ReleaseSampled(address, ignored)) {
      evidence = fmt::format("{{\"released\":false,\"sampled\":{},\"engine\":{}}}", address, ignored);
      return false;
    }
  // The buffers of the draw layer.
  for (uint64_t key : keys.buffers)
    if (!backend_->ReleaseBuffer(key, ignored)) {
      evidence = fmt::format("{{\"released\":false,\"buffer\":{},\"engine\":{}}}", key, ignored);
      return false;
    }
  std::string idle;
  const bool waited = backend_->WaitIdle(idle);
  evidence = fmt::format("{{\"released\":{},\"surfaces\":[{}],\"textures\":[{}],\"sampled\":{},\"buffers\":{},"
                         "\"idle\":{},\"counts\":{}}}", waited, fmt::join(keys.surfaces, ","),
                         fmt::join(keys.textures, ","), keys.sampled.size(), keys.buffers.size(), idle,
                         backend_->ResourceCounts());
  return waited;
}
bool Engine::ResolveSurface(const ResolveRequest& request, uint64_t& fence, std::string& evidence) {
  if (Refused(evidence)) return false;
  return backend_ ? backend_->ResolveSurface(request, fence, evidence) : NoBackend(evidence);
}
bool Engine::DrawIndexed(const DrawRequest& request, uint64_t& fence, std::string& evidence) {
  if (Refused(evidence)) return false;
  return backend_ ? backend_->DrawIndexed(request, fence, evidence) : NoBackend(evidence);
}
std::string Engine::PipelineList() { return backend_ ? backend_->PipelineList() : std::string("[]"); }
std::vector<PipelineDesc> Engine::PipelineDescs() {
  return backend_ ? backend_->PipelineDescs() : std::vector<PipelineDesc>{};
}
bool Engine::PreparePipelines(std::string& evidence) {
  if (Refused(evidence)) return false;
  return backend_ ? backend_->PreparePipelines(evidence) : NoBackend(evidence);
}
bool Engine::PrecompilePipeline(const PipelineDesc& desc, bool& present, std::string& evidence) {
  if (backend_) return backend_->PrecompilePipeline(desc, present, evidence);
  present = false;
  return NoBackend(evidence);
}
void Engine::PrefetchPipelines(const std::vector<DrawRequest>& batch) {
  if (backend_ && !failures_->Failed()) backend_->PrefetchPipelines(batch);
}
bool Engine::PostPipeline(const PipelineDesc& desc) { return backend_ && backend_->PostPipeline(desc); }
std::string Engine::PipelineSummary() { return backend_ ? backend_->PipelineSummary() : std::string(); }
bool Engine::UploadSampled(const DrawTexture& texture, std::string& evidence) {
  if (Refused(evidence)) return false;
  return backend_ ? backend_->UploadSampled(texture, evidence) : NoBackend(evidence);
}
bool Engine::UploadBuffer(const BufferUpload& upload, std::string& evidence) {
  if (Refused(evidence)) return false;
  return backend_ ? backend_->UploadBuffer(upload, evidence) : NoBackend(evidence);
}
bool Engine::ReleaseBuffer(uint64_t key, std::string& evidence) {
  return backend_ ? backend_->ReleaseBuffer(key, evidence) : NoBackend(evidence);
}
bool Engine::ReadTexture(uint32_t key, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height,
                         std::string& evidence) {
  if (Refused(evidence)) return false;
  return backend_ ? backend_->ReadTexture(key, rgba, width, height, evidence) : NoBackend(evidence);
}
bool Engine::ReadSampled(uint32_t key, uint32_t level, std::vector<uint8_t>& rows, TextureFormat& format,
                         uint32_t& width,
                         uint32_t& height, uint32_t& row_bytes, std::string& evidence) {
  if (Refused(evidence)) return false;
  return backend_ ? backend_->ReadSampled(key, level, rows, format, width, height, row_bytes, evidence)
                  : NoBackend(evidence);
}
bool Engine::ReadSurface(uint32_t key, uint64_t after_fence, std::vector<uint8_t>& rgba, uint32_t& width,
                         uint32_t& height, std::string& evidence) {
  if (Refused(evidence)) return false;
  return backend_ ? backend_->ReadSurface(key, after_fence, rgba, width, height, evidence) : NoBackend(evidence);
}
FailureChannel& Engine::Failures() const { return *failures_; }
void Engine::LatchFailure(uint64_t sequence, const std::string& evidence) {
  failures_->Retain({sequence, FailureChannel::kPostedOperation, evidence});
}
bool Engine::Latched() const { return failures_->Failed(); }
#if defined(NATIVE_RENDERER_ENGINE_FAULT_TEST)
bool Engine::SeedSampledForTest(uint32_t address, bool in_flight, std::string& evidence) {
  return backend_ ? backend_->SeedSampledForTest(address, in_flight, evidence) : NoBackend(evidence);
}
bool Engine::ReadBufferForTest(uint64_t key, std::vector<uint8_t>& bytes, std::string& evidence) {
  return backend_ ? backend_->ReadBufferForTest(key, bytes, evidence) : NoBackend(evidence);
}
CompletionTestHooks* Engine::CompletionHooksForTest() {
  return backend_ ? backend_->CompletionHooksForTest() : nullptr;
}
#endif
}  // namespace gpu
