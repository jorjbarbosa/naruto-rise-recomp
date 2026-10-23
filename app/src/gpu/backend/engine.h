#pragma once

#include <memory>
#include <string>
#include <array>
#include <cstdint>
#include <vector>
#include "gpu/backend/contracts.h"

namespace gpu {
class Backend;
class CompletionTestHooks;
class FailureChannel;

// Host engine owned for the entire accepted guest segment. No guest commands,
// read pointers, vblank or interrupts are implemented here; presentation is
// the own swapchain. The engine is the facade of the host graphics API:
// Initialize selects the backend of RenderMode::api (backend.h), and every
// operation runs on it. The session's failure channel (failure_channel.h)
// comes from the dispatcher, or is the engine's own when it stands alone.
class Engine final {
 public:
  Engine();
  explicit Engine(std::shared_ptr<FailureChannel> failures);
  ~Engine();
  bool Initialize(std::string& evidence, const RenderMode& mode = RenderMode{});
  // The API of the backend (RenderMode::api), from Initialize on.
  GraphicsApi Api() const { return api_; }
  // The device in use by its name (the DXGI description on D3D12), after Initialize.
  std::string DeviceName() const;
  bool Shutdown(std::string& evidence);
  bool InitializeFrontend(uint64_t session_generation, std::string& evidence);
  bool VerifyFrontend(uint64_t session_generation, std::string& evidence);
  bool ReleaseFrontend(uint64_t session_generation, std::string& evidence);
  // Own render-target and depth surfaces, created on the owner thread. A
  // surface is destroyed only when no submitted work can still reference it.
  bool CreateSurface(const SurfaceDesc& desc, std::string& evidence);
  bool DestroySurface(uint32_t key, std::string& evidence);
  // Keeps a surface with exactly this description, recreating it when idle.
  bool EnsureSurface(const SurfaceDesc& desc, std::string& evidence);
  // Submits without waiting; the surfaces record the operation fence.
  bool ClearSurfaces(const ClearRequest& request, uint64_t& fence, std::string& evidence);
  // Waits for every submitted operation (BlockOnFence for own work).
  bool WaitIdle(std::string& evidence);
  // Closes the open batch without waiting; the host event (FrameEvent)
  // is set once every operation submitted so far has completed (target is the
  // fence it waits for). The frame lot of D3DDevice_Swap.
  bool CloseFrame(FrameEvent event, uint64_t& target, std::string& evidence);
  // Presentation: own Plume swapchain (B8G8R8A8_UNORM; buffers and frame
  // latency from the mode) on the window (its native handles), created on the
  // engine queue.
  bool AttachWindow(const platform::NativeWindow& window, std::string& evidence, const PresentMode& mode = PresentMode{});
  // Waits for every submitted operation, then releases the swapchain.
  bool DetachWindow(std::string& evidence);
  // The title's gamma ramp: D3DGAMMARAMP words (red, green, blue x 256; the
  // 10-bit value << 6). Used by every later Present until replaced.
  bool SetGammaRamp(const std::array<uint16_t, 768>& words, std::string& evidence);
  bool SetVsync(bool enabled, std::string& evidence);
  // Copies the RGBA8 image of key (an own texture with defined content, or a
  // single-sample color surface) into a stage slot, in the open batch. A front
  // buffer with undefined content is refused. The slot must have been
  // presented since it was last staged.
  bool StageFront(uint32_t key, uint32_t& slot, std::string& evidence);
  // Presents a staged slot: black bars, the image scaled into the largest
  // rect of its aspect, the gamma ramp applied, then the swapchain Present.
  // The swapchain follows the window size. A minimized (empty) window skips
  // the frame. With capture, the back buffer is read back after the pass
  // (BGRA rows, its size in width and height) and the call waits for it.
  bool Present(uint32_t slot, bool capture, std::vector<uint8_t>& bgra, uint32_t& width, uint32_t& height,
               std::string& evidence);
  // Live own resources (textures of guest headers, surfaces, stage slots).
  std::string ResourceCounts();
  // Releases the own texture of a guest header (a resolve
  // destination) and its SRV slot once no submitted work can use them: at once
  // when its last use completed, otherwise retired with its fence and released
  // by Reclaim, WaitIdle or Shutdown. A header without an own texture is fine.
  bool ReleaseTexture(uint32_t key, std::string& evidence);
  // Releases the sampled copy of guest texture memory at a base
  // address (the key of a draw texture that is not a resolve destination) and
  // its SRV slot, as ReleaseTexture does: at once when its last use completed,
  // otherwise retired with its fence. An address without a copy is fine.
  bool ReleaseSampled(uint32_t address, std::string& evidence);
  // Ordered close: releases the own surfaces and textures no guest
  // header of the registry owns (pass backings, resolve destinations of headers
  // the title did not create through D210) and every remaining sampled
  // copy, as DestroySurface, ReleaseTexture and ReleaseSampled do, then waits
  // for the queue so nothing stays retired.
  bool ReleaseRemaining(std::string& evidence);
  // Copies or resolves a surface into the own texture of a guest header.
  bool ResolveSurface(const ResolveRequest& request, uint64_t& fence, std::string& evidence);
  // Draws into an own surface with an own pipeline; submits without waiting.
  bool DrawIndexed(const DrawRequest& request, uint64_t& fence, std::string& evidence);
  // The pipelines created so far, one JSON object each, with the stable
  // identities of their key (shader names, layout id and elements, state
  // words): the list a build step can turn into the precompiled set.
  std::string PipelineList();
  // The created pipelines as descriptions (no DXIL); their key is
  // PipelineKey (pipeline_keys.h), as DrawIndexed computes it.
  std::vector<PipelineDesc> PipelineDescs();
  // Precompiled pipelines. PreparePipelines (owner) creates the draw root
  // signature and heaps once. PrecompilePipeline then runs on any thread,
  // concurrently with the owner: it creates the pipeline of desc into the
  // precompiled set unless the owner or another thread has its key already
  // (present). DrawIndexed takes a ready pipeline instead of creating one and
  // waits for one being created; after a failure (evidence) it creates its own.
  bool PreparePipelines(std::string& evidence);
  bool PrecompilePipeline(const PipelineDesc& desc, bool& present, std::string& evidence);
  // The pipelines of a draw batch that are neither created nor in the
  // pipeline set, posted to the asynchronous threads before the batch is
  // drawn (requests of PipelineTake::kPrefetch). Owner.
  void PrefetchPipelines(const std::vector<DrawRequest>& batch);
  // Posts one description of the list to the asynchronous threads unless the
  // pipeline set has its key already (the pool compiled it, or a draw took it):
  // the pipelines of the world the title starts loading
  // (shaders/pipelines.cpp). Any thread once PreparePipelines ran, as
  // PrecompilePipeline; false when not posted.
  bool PostPipeline(const PipelineDesc& desc);
  // How the pipelines were taken since Initialize, as JSON fields (any thread).
  std::string PipelineSummary();
  // Uploads the levels of a sampled copy (key, layout, version, data)
  // into the open batch, before the draws recorded after it; recreated when
  // its description changes, as in DrawIndexed. A version not above the
  // copy's is skipped (a later upload already ran).
  bool UploadSampled(const DrawTexture& texture, std::string& evidence);
  // Creates or refreshes a vertex or index buffer of the caller's key
  // in the open batch (see BufferUpload), and releases one as
  // ReleaseSampled does: at once when its last use completed, otherwise
  // retired with its fence. An unknown key is fine for the release.
  bool UploadBuffer(const BufferUpload& upload, std::string& evidence);
  bool ReleaseBuffer(uint64_t key, std::string& evidence);
  // Reads back the own RGBA8 texture of a guest header after every submitted
  // operation (front-buffer evidence of D3DDevice_Swap). Waits for the copy.
  // A depth resolve destination gives its R32_FLOAT texels, 4 bytes each.
  bool ReadTexture(uint32_t key, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height,
                   std::string& evidence);
  // Frame trace (diagnosis): reads back one level (face 0) of a sampled copy
  // after every submitted operation, as its rows (block rows for the BC
  // formats) of row_bytes each. Waits for the copy.
  bool ReadSampled(uint32_t key, uint32_t level, std::vector<uint8_t>& rows, TextureFormat& format, uint32_t& width,
                   uint32_t& height, uint32_t& row_bytes, std::string& evidence);
  // Reads back an own single-sample RGBA8 surface whose last use is the
  // operation of after_fence, once that fence has completed (isolated START
  // target). Waits for the copy.
  bool ReadSurface(uint32_t key, uint64_t after_fence, std::vector<uint8_t>& rgba, uint32_t& width,
                   uint32_t& height, std::string& evidence);
  // Once the session's failure channel holds a failure, every GPU operation
  // is refused with its evidence: nothing is recorded or closed and the open
  // batch is never submitted. WaitIdle still waits for the batches submitted
  // before and then reports false, so no fence reaches the title. Lifecycle
  // (frontend, Shutdown) stays available. LatchFailure retains the failure of
  // a posted operation; Latched answers from any thread.
  FailureChannel& Failures() const;
  void LatchFailure(uint64_t sequence, const std::string& evidence);
  bool Latched() const;
#if defined(NATIVE_RENDERER_ENGINE_FAULT_TEST)
  // Host test: a sampled copy at address (a 4 x 4 RGBA8 resource with an
  // SRV slot), as a draw leaves it, last used by the open batch when in_flight.
  bool SeedSampledForTest(uint32_t address, bool in_flight, std::string& evidence);
  // Host test: the bytes of a buffer of the draw layer (UploadBuffer), read
  // back after every submitted operation. Waits for the copy.
  bool ReadBufferForTest(uint64_t key, std::vector<uint8_t>& bytes, std::string& evidence);
  // Host test: the hooks of the backend's completion service (backend.h), or
  // null when the backend has none.
  CompletionTestHooks* CompletionHooksForTest();
#endif
 private:
  // The refusal of a GPU operation after a retained failure.
  bool Refused(std::string& evidence) const;
  const std::shared_ptr<FailureChannel> failures_;
  std::unique_ptr<Backend> backend_;
  GraphicsApi api_ = GraphicsApi::kD3D12;
};
}  // namespace gpu
