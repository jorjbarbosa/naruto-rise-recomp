// One host graphics API under the engine: the engine (engine.h) selects a
// backend at Initialize and runs every operation on it. The operations, their
// threads and their evidence are those of engine.h; a backend keeps the API's
// own resources, commands and completion. The backends are backend/d3d12 and
// backend/vulkan.
//
// The engine refuses every GPU operation once the session's failure channel
// holds a failure (failure_channel.h). A backend submits its command lists
// only through FailureChannel::Authorize, so nothing is submitted after a
// failure, and retains in the channel the failures its own services find.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "gpu/backend/failure_channel.h"
#include "gpu/backend/contracts.h"

namespace gpu {

#if defined(NATIVE_RENDERER_ENGINE_FAULT_TEST)
// Host tests of a backend's completion service (the Vulkan one): its next
// submissions held on the GPU until released, a result injected into its next
// wait, a shorter deadline, and the count of its submissions.
class CompletionTestHooks {
 public:
  virtual ~CompletionTestHooks() = default;
  // Owner: the submissions from now on wait on the GPU until the release
  // (any thread). False when the device cannot hold them.
  virtual bool HoldSubmissions(std::string& evidence) = 0;
  virtual bool ReleaseSubmissions(std::string& evidence) = 0;
  virtual void InjectResult(int32_t result) = 0;
  virtual void SetDeadline(uint32_t milliseconds) = 0;
  virtual uint64_t Submissions() const = 0;
};
#endif

// The keys of a backend's live own resources, each list sorted.
struct ResourceKeys {
  std::vector<uint32_t> surfaces, textures, sampled;
  std::vector<uint64_t> buffers;
};

class Backend {
 public:
  virtual ~Backend() = default;
  virtual bool Initialize(std::string& evidence, const RenderMode& mode) = 0;
  virtual std::string DeviceName() const = 0;
  virtual bool Shutdown(std::string& evidence) = 0;
  virtual bool InitializeFrontend(uint64_t session_generation, std::string& evidence) = 0;
  virtual bool VerifyFrontend(uint64_t session_generation, std::string& evidence) = 0;
  virtual bool ReleaseFrontend(uint64_t session_generation, std::string& evidence) = 0;
  virtual bool CreateSurface(const SurfaceDesc& desc, std::string& evidence) = 0;
  virtual bool DestroySurface(uint32_t key, std::string& evidence) = 0;
  virtual bool EnsureSurface(const SurfaceDesc& desc, std::string& evidence) = 0;
  virtual bool ClearSurfaces(const ClearRequest& request, uint64_t& fence, std::string& evidence) = 0;
  virtual bool WaitIdle(std::string& evidence) = 0;
  // Waits for the work submitted so far without closing the open batch: the
  // wait of WaitIdle once a failure was retained.
  virtual bool WaitSubmitted(std::string& evidence) = 0;
  virtual bool CloseFrame(void* event, uint64_t& target, std::string& evidence) = 0;
  virtual bool AttachWindow(void* window, std::string& evidence, const PresentMode& mode) = 0;
  virtual bool DetachWindow(std::string& evidence) = 0;
  virtual bool SetGammaRamp(const std::array<uint16_t, 768>& words, std::string& evidence) = 0;
  virtual bool SetVsync(bool enabled, std::string& evidence) = 0;
  // Owner, after frame completion. Resolved textures and guest resources survive.
  // Rebuilds transient render targets only when their dimensions/samples change.
  virtual bool Reconfigure(const RenderMode&, const PresentMode&, std::string& evidence) = 0;
  virtual bool StageFront(uint32_t key, uint32_t& slot, std::string& evidence) = 0;
  virtual bool Present(uint32_t slot, bool capture, std::vector<uint8_t>& bgra, uint32_t& width, uint32_t& height,
                       std::string& evidence, bool& presented) = 0;
  virtual bool UploadUiTexture(uint32_t key, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba,
                               std::string& evidence) = 0;
  virtual bool ReleaseUiTexture(uint32_t key, std::string& evidence) = 0;
  virtual bool SetUiFrame(UiFrame frame, std::string& evidence) = 0;
  virtual std::string ResourceCounts() = 0;
  virtual bool ReleaseTexture(uint32_t key, std::string& evidence) = 0;
  virtual bool ReleaseSampled(uint32_t address, std::string& evidence) = 0;
  // The resources Engine::ReleaseRemaining releases; refused off the owner thread.
  virtual bool LiveResources(ResourceKeys& keys, std::string& evidence) = 0;
  virtual bool ResolveSurface(const ResolveRequest& request, uint64_t& fence, std::string& evidence) = 0;
  virtual bool DrawIndexed(const DrawRequest& request, uint64_t& fence, std::string& evidence) = 0;
  virtual std::string PipelineList() = 0;
  virtual std::vector<PipelineDesc> PipelineDescs() = 0;
  virtual bool PreparePipelines(std::string& evidence) = 0;
  virtual bool PrecompilePipeline(const PipelineDesc& desc, bool& present, std::string& evidence) = 0;
  virtual void PrefetchPipelines(const std::vector<DrawRequest>& batch) = 0;
  virtual bool PostPipeline(const PipelineDesc& desc) = 0;
  virtual std::string PipelineSummary() = 0;
  virtual bool UploadSampled(const DrawTexture& texture, std::string& evidence) = 0;
  virtual bool UploadBuffer(const BufferUpload& upload, std::string& evidence) = 0;
  virtual bool ReleaseBuffer(uint64_t key, std::string& evidence) = 0;
  virtual bool ReadTexture(uint32_t key, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height,
                           std::string& evidence) = 0;
  virtual bool ReadSampled(uint32_t key, uint32_t level, std::vector<uint8_t>& rows, TextureFormat& format,
                           uint32_t& width, uint32_t& height, uint32_t& row_bytes, std::string& evidence) = 0;
  virtual bool ReadSurface(uint32_t key, uint64_t after_fence, std::vector<uint8_t>& rgba, uint32_t& width,
                           uint32_t& height, std::string& evidence) = 0;
#if defined(NATIVE_RENDERER_ENGINE_FAULT_TEST)
  virtual bool SeedSampledForTest(uint32_t address, bool in_flight, std::string& evidence) = 0;
  virtual bool ReadBufferForTest(uint64_t key, std::vector<uint8_t>& bytes, std::string& evidence) = 0;
  // The hooks of the backend's completion service, when it has one.
  virtual CompletionTestHooks* CompletionHooksForTest() { return nullptr; }
#endif
};

// The backends compiled into the app, each bound to the session's failure
// channel. The Vulkan one exists in builds with NARUTORISE_NATIVE_VULKAN.
std::unique_ptr<Backend> CreateD3d12Backend(std::shared_ptr<FailureChannel> failures);
std::unique_ptr<Backend> CreateVulkanBackend(std::shared_ptr<FailureChannel> failures);

}  // namespace gpu
