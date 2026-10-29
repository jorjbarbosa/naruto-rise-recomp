// The D3D12 backend of the engine (backend.h). Plume creates the device, the
// queue, the fence and the swapchain; the resources, commands and passes are
// D3D12's own.
#include "gpu/backend/backend.h"
#include "gpu/backend/reconfigure.h"
#include "gpu/backend/output_plan.h"
#include "gpu/backend/d3d12/formats.h"
#include "gpu/backend/lifecycle.h"
#include "gpu/backend/pipeline_keys.h"
#include "gpu/backend/pipeline_set.h"
#include "gpu/backend/regions.h"
#include "gpu/common/hash.h"
#include "gpu/common/title_loading.h"
#include "gpu/d3d/init/initializer_services.h"

#include <windows.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <cstdlib>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <mutex>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <span>
#include <deque>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <string_view>
#include <thread>
#include <chrono>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include "plume_d3d12.h"
#include "gpu/diagnostics/profile.h"
#include "gpu/backend/present_pass.h"  // kPresentVs, kPresentPs (DXIL).
#include "gpu/backend/depth_resolve.h"  // kDepthResolveVs, ...MsPs, ...SinglePs (DXIL).
#include "gpu/backend/sample_resolve.h"  // kSampleResolvePs (DXIL).
#include "gpu/backend/sdk_shaders.h"   // CAS, FSR1, bilinear and FXAA of the SDK (DXBC).
#include "gpu/backend/ui_pass.h"  // kUiVs, kUiPs (DXIL).

namespace plume { std::unique_ptr<RenderInterface> CreateD3D12Interface(); }

namespace gpu::d3d12 {
namespace {
constexpr size_t kDrawStreamsMax = 8;  // Vertex buffer slots (the title binds up to 4).
// REX_NATIVE_D3D12_DEBUG=1 turns the D3D12 debug layer on; without it
// no info queue exists and no diagnostic can be stored or read. Unset or 0
// leaves it off, the product default: its validation costs about a third of
// the frames in the heavy scenes. The engine host tests ask for it in their
// main. Any other value is refused.
int debugLayerOption() {
  static const int value = [] {
    const char* text = std::getenv("REX_NATIVE_D3D12_DEBUG");
    if (!text || !*text || std::string_view(text) == "0") return 0;
    return std::string_view(text) == "1" ? 1 : -1;
  }();
  return value;
}
// A persistent target accepts different clear colors. D3D12 warning 820 is
// solely an optimized-clear performance diagnostic; retain it in the queue.
// Every other message, a different severity/category, or unreadable diagnostics
// still rejects the operation. Initialization retains its zero-message gate.
struct ColorDebugStatus {
  uint64_t total = 0, expected_clear_warnings = 0, unexpected = 0;
};
ColorDebugStatus colorDebugStatus(ID3D12InfoQueue* info, bool colorTargetExists) {
  ColorDebugStatus status;
  if (!info) { status.unexpected = debugLayerOption() == 0 ? 0 : 1; return status; }
  status.total = info->GetNumStoredMessagesAllowedByRetrievalFilter();
  if (status.total > 64) { status.unexpected = status.total; return status; }
  // A message dropped by the queue limit cannot be proved accepted.
  if (const uint64_t dropped = info->GetNumMessagesDiscardedByMessageCountLimit()) {
    status.unexpected = dropped;
    return status;
  }
  for (uint64_t i = 0; i < status.total; ++i) {
    SIZE_T bytes = 0;
    if (FAILED(info->GetMessage(i, nullptr, &bytes)) || bytes < sizeof(D3D12_MESSAGE)) {
      ++status.unexpected; continue;
    }
    std::vector<uint8_t> storage(bytes);
    auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
    if (FAILED(info->GetMessage(i, message, &bytes))) { ++status.unexpected; continue; }
    if (colorTargetExists && (message->ID == D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE ||
                              message->ID == D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE) &&
        message->Severity == D3D12_MESSAGE_SEVERITY_WARNING && message->Category == D3D12_MESSAGE_CATEGORY_EXECUTION)
      ++status.expected_clear_warnings;
    else ++status.unexpected;
  }
  return status;
}
// Keep rejected GPU work diagnosable without clearing or filtering messages.
struct DebugArchive {
  std::string ids;
  uint64_t copied = 0;
  bool complete = false;
};
DebugArchive debugMessageIds(ID3D12InfoQueue* info) {
  DebugArchive archive;
  if (!info) return DebugArchive{"[]", 0, debugLayerOption() == 0};
  std::string ids = "[";
  const uint64_t count = info->GetNumStoredMessagesAllowedByRetrievalFilter();
  for (uint64_t i = 0; i < count && i < 64; ++i) {
    SIZE_T bytes = 0;
    if (FAILED(info->GetMessage(i, nullptr, &bytes)) || bytes < sizeof(D3D12_MESSAGE)) break;
    std::vector<uint8_t> storage(bytes);
    auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
    if (FAILED(info->GetMessage(i, message, &bytes))) break;
    ids += fmt::format("{}{{\"id\":{},\"severity\":{},\"category\":{}}}",
        ids.size() == 1 ? "" : ",", static_cast<uint32_t>(message->ID),
        static_cast<uint32_t>(message->Severity), static_cast<uint32_t>(message->Category));
    ++archive.copied;
  }
  archive.ids = ids + "]";
  archive.complete = count <= 64 && archive.copied == count;
  return archive;
}
// Every own clear of the D3D boundary stores the accepted clear
// warnings, so the frame loop passes the 64 stored messages that the release
// accepts. While the stored messages are only accepted warnings they are
// counted and cleared; any other message stays stored and uncleared, and keeps
// the release rejected with its IDs archived.
void archiveClearWarnings(ID3D12InfoQueue* info, bool colorTargetExists, uint64_t& archived) {
  if (!info) return;
  const auto status = colorDebugStatus(info, colorTargetExists);
  if (!status.total || status.unexpected || status.expected_clear_warnings != status.total) return;
  archived += status.total;
  info->ClearStoredMessages();
}
// The chain of output effects after the ramp, as the SDK's
// Presenter::GetGuestOutputPaintFlow: FSR upscales with EASU passes of up to 2x
// each, then RCAS at the reached size; CAS (and FSR without upscaling) sharpens
// at the same size or resamples up to 2x; a bilinear pass stretches the rest.

}

class Backend final : public gpu::Backend {
 public:
  explicit Backend(std::shared_ptr<FailureChannel> failures);
  ~Backend() override;
  bool Initialize(std::string& evidence, const RenderMode& mode) override;
  std::string DeviceName() const override;
  bool Shutdown(std::string& evidence) override;
  bool InitializeFrontend(uint64_t session_generation, std::string& evidence) override;
  bool VerifyFrontend(uint64_t session_generation, std::string& evidence) override;
  bool ReleaseFrontend(uint64_t session_generation, std::string& evidence) override;
  bool CreateSurface(const SurfaceDesc& desc, std::string& evidence) override;
  bool DestroySurface(uint32_t key, std::string& evidence) override;
  bool EnsureSurface(const SurfaceDesc& desc, std::string& evidence) override;
  bool ClearSurfaces(const ClearRequest& request, uint64_t& fence, std::string& evidence) override;
  bool WaitIdle(std::string& evidence) override;
  bool WaitSubmitted(std::string& evidence) override;
  bool CloseFrame(FrameEvent event, uint64_t& target, std::string& evidence) override;
  bool AttachWindow(const platform::NativeWindow& window, std::string& evidence, const PresentMode& mode) override;
  bool DetachWindow(std::string& evidence) override;
  bool SetGammaRamp(const std::array<uint16_t, 768>& words, std::string& evidence) override;
  bool SetVsync(bool enabled, std::string& evidence) override;
  bool Reconfigure(const RenderMode&, const PresentMode&, std::string& evidence) override;
  bool StageFront(uint32_t key, uint32_t& slot, std::string& evidence) override;
  bool Present(uint32_t slot, bool capture, std::vector<uint8_t>& bgra, uint32_t& width, uint32_t& height,
               std::string& evidence, bool& presented) override;
  bool UploadUiTexture(uint32_t key, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba,
                       std::string& evidence) override;
  bool ReleaseUiTexture(uint32_t key, std::string& evidence) override;
  bool SetUiFrame(UiFrame frame, std::string& evidence) override;
  std::string ResourceCounts() override;
  bool ReleaseTexture(uint32_t key, std::string& evidence) override;
  bool ReleaseSampled(uint32_t address, std::string& evidence) override;
  bool LiveResources(ResourceKeys& keys, std::string& evidence) override;
  bool ResolveSurface(const ResolveRequest& request, uint64_t& fence, std::string& evidence) override;
  bool DrawIndexed(const DrawRequest& request, uint64_t& fence, std::string& evidence) override;
  std::string PipelineList() override;
  std::vector<PipelineDesc> PipelineDescs() override;
  bool PreparePipelines(std::string& evidence) override;
  bool PrecompilePipeline(const PipelineDesc& desc, bool& present, std::string& evidence) override;
  void PrefetchPipelines(const std::vector<DrawRequest>& batch) override;
  bool PostPipeline(const PipelineDesc& desc) override;
  std::string PipelineSummary() override;
  bool UploadSampled(const DrawTexture& texture, std::string& evidence) override;
  bool UploadBuffer(const BufferUpload& upload, std::string& evidence) override;
  bool ReleaseBuffer(uint64_t key, std::string& evidence) override;
  bool ReadTexture(uint32_t key, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height,
                   std::string& evidence) override;
  bool ReadSampled(uint32_t key, uint32_t level, std::vector<uint8_t>& rows, TextureFormat& format, uint32_t& width,
                   uint32_t& height, uint32_t& row_bytes, std::string& evidence) override;
  bool ReadSurface(uint32_t key, uint64_t after_fence, std::vector<uint8_t>& rgba, uint32_t& width,
                   uint32_t& height, std::string& evidence) override;
#if defined(NATIVE_RENDERER_ENGINE_FAULT_TEST)
  bool SeedSampledForTest(uint32_t address, bool in_flight, std::string& evidence) override;
  bool ReadBufferForTest(uint64_t key, std::vector<uint8_t>& bytes, std::string& evidence) override;
#endif
 private:
  // WaitIdle (close: the open batch is submitted first) and WaitSubmitted.
  bool Wait(bool close, std::string& evidence);
  struct State;
  std::unique_ptr<State> state_;
};

struct Backend::State {
  gpu::InitializerServices frontend;
  uint64_t archived_clear_warnings = 0;  // Accepted warnings counted and cleared.
  std::unique_ptr<plume::RenderInterface> render;
  std::unique_ptr<plume::RenderDevice> device;
  std::unique_ptr<plume::RenderCommandQueue> queue;
  std::unique_ptr<plume::RenderCommandFence> fence;
  Microsoft::WRL::ComPtr<ID3D12InfoQueue> info;
  uint64_t completed = 0;
  uint64_t pending_signal = 0;
  bool ready = false;
  bool operation_poisoned = false;
  bool unproved_execution = false;
  DWORD owner_thread = 0;
  uint64_t epoch = 0;
  struct Surface {
    SurfaceDesc desc;
    std::unique_ptr<plume::RenderTexture> texture;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> view;  // RTV or DSV
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    uint64_t last_use_fence = 0;  // Highest fence of submitted work using it.
    SurfaceDesc guest;
  };
  struct Submission {  // Retained until its fence completes.
    uint64_t fence = 0;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;  // Draw data, kept until the fence.
  };
  std::vector<Submission> submissions;  // Readbacks.
  // Draws, clears and resolves are recorded into one open
  // batch: a pooled allocator, the reused command list and persistent upload
  // pages. A batch closes with one ExecuteCommandLists and one Signal; until
  // its fence completes it keeps its allocator and upload pages, and the
  // resources and view slots replaced while it could still read them.
  struct UploadPage {
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    uint8_t* cpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    size_t size = 0, used = 0;
  };
  struct Batch {
    uint64_t fence = 0;  // The Signal of its close, known while it records.
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    std::vector<UploadPage> pages;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> retained;
    std::vector<uint32_t> views;  // SRV slots back to free_views at the fence.
    uint64_t operations = 0;
    // False after Reset or a presentation/resolve pass changes graphics state.
    // Clears, copies and resource barriers leave the bindings intact.
    bool draw_bindings = false;
    struct DrawBindings {
      ID3D12PipelineState* pipeline = nullptr;
      ID3D12Resource* color = nullptr;
      ID3D12Resource* depth = nullptr;
      D3D12_CPU_DESCRIPTOR_HANDLE rtv{}, dsv{};
      D3D12_VIEWPORT viewport{};
      D3D12_RECT scissor{};
      std::array<D3D12_GPU_VIRTUAL_ADDRESS, 3> constants{};
      std::array<D3D12_VERTEX_BUFFER_VIEW, kDrawStreamsMax> streams{};
      UINT stream_count = 0;
      D3D12_INDEX_BUFFER_VIEW indices{};
      D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
      std::array<float, 4> blend{};
      UINT stencil_ref = 0;
      bool stencil_valid = false;
    } bound;
  };
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> batch_list;
  bool batch_open = false;
  Batch batch;
  // The constant CBVs of the previous draw of the open batch, reused
  // while the request's version holds (its upload page lives until the fence),
  // and the shared block's, reused while its bytes are the same.
  uint64_t cbv_batch_fence = 0, last_vs_version = 0, last_ps_version = 0;
  D3D12_GPU_VIRTUAL_ADDRESS last_vs_cbv = 0, last_ps_cbv = 0, last_shared_cbv = 0;
  std::array<uint8_t, 512> last_shared{};
  // The previous draw of the open batch, for the fusion counters of the
  // profile (diagnostics only): its batch and operation number (fence 0 when
  // it recorded no draw), its bindings, shared block and base vertex.
  struct FusionProbe {
    uint64_t fence = 0, operation = 0;
    Batch::DrawBindings bound;
    std::array<uint8_t, 512> shared{};
    INT base_vertex = 0;
  } fusion_probe;
  std::deque<Batch> batches_in_flight;
  std::vector<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>> free_allocators;
  std::vector<UploadPage> free_pages;
  uint64_t batches_closed = 0, upload_pages_created = 0, allocators_created = 0;
  // The session's failure channel: every command list is submitted through
  // it (Submit), so nothing is submitted once it holds a failure. A batch
  // closed but refused that way is never submitted (batch_refused), like the
  // open batch Shutdown discards.
  std::shared_ptr<FailureChannel> failures;
  bool batch_refused = false;
  bool OpenBatch(const char*& reason, HRESULT& hr);
  bool CloseBatch(const char*& reason, HRESULT& hr);
  // Executes a closed command list and signals fence_value after it, unless
  // the session holds a failure (false, hr E_ABORT). A failed Signal poisons
  // the owner.
  bool Submit(ID3D12GraphicsCommandList* list, uint64_t fence_value, HRESULT& hr);
  void Reclaim(uint64_t completed);
  bool Upload(size_t bytes, UploadPage*& page, size_t& offset, HRESULT& hr);
  // Draw layer: XenosRecomp root signature, shader-visible heaps
  // and pipelines keyed by shaders and state.
  Microsoft::WRL::ComPtr<ID3D12RootSignature> draw_root;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> draw_views, draw_samplers;
  std::unordered_map<uint64_t, PipelineSet::Pipeline> pipelines;
  // The stable identity of every created pipeline (PipelineList): the
  // shader names of the cache, the layout id and its elements, and the state
  // words of the key, in the order the key hashes them.
  // Origin is how the draw got it: "created" by itself, "warm" (by itself, a
  // triple compiled before), "precompiled" (the list), "async" (the asynchronous
  // threads) or "waited" (for either creating it); see pipeline_set.h.
  struct PipelineRecord {
    uint64_t key = 0, layout_id = 0;
    std::string vs, ps;
    VertexElements elements;
    std::array<uint32_t, 21> state{};
    const char* origin = "created";
    uint32_t create_us = 0;  // The creation by the draw ("created" or "warm"), 0 otherwise.
    uint32_t world = 0;      // The world loaded last when the draw took it (common/title_loading.h).
  };
  std::vector<PipelineRecord> pipeline_records;
  // The pipelines between their creation and the owner's take: the embedded
  // list's (the pool threads), the asynchronous threads' and the takes of the
  // draws (pipeline_set.h). It creates through the draw layer (EnsureDrawLayer).
  PipelineSet pipeline_set;
  // Draw root signature (XenosRecomp layout) and shader-visible heaps, once.
  bool EnsureDrawLayer(const char*& reason, HRESULT& hr);
  // A free SRV slot of the draw heap; false when all are taken.
  bool AllocateDrawView(uint32_t& slot);
  // The holders of the draw heap, for the evidence of an exhausted heap.
  std::string DrawViewCounts() const;
  // The constant blocks of the draw layer, on the owner thread: the requests
  // bring the whole block or the registers written since the previous request.
  std::array<uint8_t, 4096> vs_block{}, ps_block{};
  uint64_t draws = 0;
  struct Texture {  // Own texture of a guest texture header.
    std::unique_ptr<plume::RenderTexture> texture;
    uint32_t width = 0, height = 0;
    bool depth = false, content_defined = false;
    bool render_target = false;  // Created for the resolve passes (depth, one color sample).
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    uint64_t last_use_fence = 0;
    uint32_t view = 0;  // Shader-visible SRV slot once sampled; 0 = none.
    ID3D12Resource* view_of = nullptr;  // Resource the SRV in the slot describes.
  };
  std::unordered_map<uint32_t, Texture> textures;
  // Released textures waiting for the fence of their last use.
  std::vector<Texture> retired_textures;
  uint64_t textures_released = 0, textures_retired = 0;
  void ReleaseRetiredTextures(uint64_t completed) {
    std::erase_if(retired_textures, [&](Texture& t) {
      if (completed == UINT64_MAX || completed < t.last_use_fence) return false;
      if (t.view) free_views.push_back(t.view);
      ++textures_released;
      return true;
    });
    std::erase_if(retired_sampled, [&](Sampled& s) {
      if (completed == UINT64_MAX || completed < s.last_use_fence) return false;
      if (s.view) free_views.push_back(s.view);
      ++sampled_released;
      return true;
    });
    std::erase_if(retired_buffers, [&](Buffer& b) {
      if (completed == UINT64_MAX || completed < b.last_use_fence) return false;
      b.resource.Reset();
      FreeBufferBlocks(b);
      ++buffers_released;
      return true;
    });
    std::erase_if(retired_ui_textures, [&](UiTexture& t) {
      if (completed == UINT64_MAX || completed < t.last_use_fence) return false;
      free_ui_views.push_back(t.view);
      return true;
    });
  }
  // Sampled copies of guest texture memory, keyed by the caller.
  struct Sampled {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    TextureFormat format = TextureFormat::kUnknown;
    uint32_t width = 0, height = 0, levels = 0, view = 0;
    uint64_t layout = 0, version = 0;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
    uint64_t last_use_fence = 0;
    ID3D12Resource* view_of = nullptr;  // Resource the SRV in the slot describes.
    bool cube = false;                  // Six array slices.
  };
  std::unordered_map<uint32_t, Sampled> sampled;
  // Sampled copies of destroyed headers waiting for the fence of their last use.
  std::vector<Sampled> retired_sampled;
  uint64_t sampled_released = 0, sampled_retired = 0;
  // Vertex and index buffers of the draw layer (UploadBuffer), keyed by
  // the caller; released at once or retired with the fence of their last use.
  // A buffer is a placed resource in one of the pool's 64 MB heaps (64 KB
  // blocks, first fit): a committed resource per promoted range costs ~0.5 ms
  // each, in bursts of ~80 per frame when the camera reveals new meshes. Its
  // blocks are freed once its last use completed.
  struct Buffer {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    uint32_t bytes = 0;
    uint64_t version = 0, last_use_fence = 0;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
    uint32_t heap = 0, block = 0, blocks = 0;
  };
  std::unordered_map<uint64_t, Buffer> buffers;
  std::vector<Buffer> retired_buffers;
  uint64_t buffers_created = 0, buffers_released = 0, buffers_retired = 0, buffer_uploads = 0;
  static constexpr uint32_t kBufferHeapBytes = 64u << 20, kBufferBlockBytes = 64u << 10;
  static constexpr uint32_t kBufferHeapBlocks = kBufferHeapBytes / kBufferBlockBytes;
  struct BufferHeap {
    Microsoft::WRL::ComPtr<ID3D12Heap> heap;
    std::vector<uint8_t> used;  // One flag per block.
    uint32_t free_blocks = kBufferHeapBlocks;
  };
  std::vector<BufferHeap> buffer_heaps;
  uint64_t buffer_heaps_created = 0, buffer_blocks_used = 0;
  void FreeBufferBlocks(const Buffer& b) {
    if (!b.blocks || b.heap >= buffer_heaps.size()) return;
    auto& h = buffer_heaps[b.heap];
    for (uint32_t i = 0; i < b.blocks; ++i) h.used[b.block + i] = 0;
    h.free_blocks += b.blocks;
    buffer_blocks_used -= b.blocks;
  }
  // First fit of blocks contiguous blocks in any heap, else a new heap. False: hr.
  bool AllocateBufferBlocks(uint32_t blocks, uint32_t& heap_index, uint32_t& block, HRESULT& hr) {
    hr = S_OK;
    if (!blocks || blocks > kBufferHeapBlocks) { hr = E_INVALIDARG; return false; }
    for (uint32_t h = 0; h < buffer_heaps.size(); ++h) {
      auto& heap = buffer_heaps[h];
      if (heap.free_blocks < blocks) continue;
      uint32_t run = 0;
      for (uint32_t i = 0; i < kBufferHeapBlocks; ++i) {
        run = heap.used[i] ? 0 : run + 1;
        if (run < blocks) continue;
        heap_index = h;
        block = i + 1 - blocks;
        for (uint32_t j = 0; j < blocks; ++j) heap.used[block + j] = 1;
        heap.free_blocks -= blocks;
        buffer_blocks_used += blocks;
        return true;
      }
    }
    BufferHeap next;
    D3D12_HEAP_DESC desc{};
    desc.SizeInBytes = kBufferHeapBytes;
    desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    desc.Properties.CreationNodeMask = desc.Properties.VisibleNodeMask = 1;
    desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    auto* d3d = static_cast<plume::D3D12Device*>(device.get())->d3d;
    hr = d3d->CreateHeap(&desc, IID_PPV_ARGS(&next.heap));
    if (FAILED(hr)) return false;
    next.used.assign(kBufferHeapBlocks, 0);
    for (uint32_t j = 0; j < blocks; ++j) next.used[j] = 1;
    next.free_blocks -= blocks;
    buffer_blocks_used += blocks;
    heap_index = uint32_t(buffer_heaps.size());
    block = 0;
    buffer_heaps.push_back(std::move(next));
    ++buffer_heaps_created;
    return true;
  }
  // The state of a resource the draws' shaders read. NVIDIA's also names
  // COPY_SOURCE: the shader resource states alone were seen to miss cache
  // flushes and layout transitions there (Dawn's
  // d3d12_expand_shader_resource_state_transitions_to_copy_source).
  D3D12_RESOURCE_STATES shader_read =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  // Fixed descriptor slots: a slot is rewritten only when no submitted work
  // can still read it. Slot 0 holds the null view and the point sampler.
  uint32_t next_view = 1;
  std::vector<uint32_t> free_views;
  // A sampler's slot by its description; with the heap full, a new one takes
  // the slot of a sampler whose last use completed (sampler_slot_use).
  std::unordered_map<uint64_t, uint32_t> sampler_slots;
  std::vector<uint64_t> sampler_slot_keys, sampler_slot_use;  // By slot.
  uint64_t operations = 0;
  std::unordered_map<uint32_t, Surface> surfaces;
  std::vector<Surface> retired_surfaces;  // Released, waiting for their fence.
  // Guest pixels and sample counts to those of the own surfaces and resolve
  // destinations (RenderMode::Host); a surface keeps its description in the latter.
  RenderMode render_mode;
  // Pipeline warmup is free-threaded; the rest of render_mode is owner-only.
  std::atomic<uint32_t> pipeline_msaa{0};
  uint32_t PipelineSamples(uint32_t guest) const {
    const uint32_t count = pipeline_msaa.load(); return guest > 1 && count ? count : guest;
  }
  // Presentation: own swapchain on the window, the final pass and two
  // stage slots holding the front buffer copied at the Swap until its flip.
  static constexpr uint32_t kMaxSwapBuffers = 3, kStageSlots = 2;
  std::unique_ptr<plume::RenderSwapChain> swapchain;
  void* window = nullptr;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> present_rtv;    // One RTV per back buffer.
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> present_views;  // Stage SRVs, then the effect views.
  Microsoft::WRL::ComPtr<ID3D12RootSignature> present_root;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> present_pipeline;
  // The output effects (OutputPlan). Target 0 holds the ramp at the source
  // size (R10G10B10A2, the SDK's guest output; R16G16B16A16 with the luma when
  // FXAA follows), target 1 the FXAA result, targets 2 on the chain's
  // intermediates. Each has an SRV and a UAV slot after the stage SRVs in
  // present_views and an RTV in effect_rtv.
  struct EffectTarget {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  };
  static constexpr uint32_t kEffectTargets = 7, kPresentViews = kStageSlots + 2 * kEffectTargets;
  std::array<EffectTarget, kEffectTargets> effect_targets;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> effect_rtv;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> effect_root, fxaa_root;
  std::map<std::pair<const void*, DXGI_FORMAT>, Microsoft::WRL::ComPtr<ID3D12PipelineState>> effect_pipelines;
  bool EnsureEffectTarget(uint32_t i, uint32_t width, uint32_t height, DXGI_FORMAT format, bool uav, HRESULT& hr);
  bool EffectRoots(HRESULT& hr);
  ID3D12PipelineState* EffectPipeline(const void* vs, size_t vs_size, const void* ps, size_t ps_size,
                                      ID3D12RootSignature* root, DXGI_FORMAT format, HRESULT& hr);
  bool OutputEffects(ID3D12GraphicsCommandList* commands, uint32_t slot, D3D12_CPU_DESCRIPTOR_HANDLE back_rtv,
                     uint32_t back_width, uint32_t back_height, uint32_t x, uint32_t y,
                     const std::vector<OutputStep>& plan, D3D12_GPU_VIRTUAL_ADDRESS ramp, HRESULT& hr);
  void ReleaseEffects() {
    for (auto& target : effect_targets) target = {};
  }
  // Depth resolve pass (single-sample and MSAA source), and the color resolve
  // of one sample (sample_resolve) with the same root signature and slots.
  // Its SRV takes a slot of the draw heap, back at the batch's fence as a
  // draw's are (the batch keeps one shader-visible heap); the destination RTV
  // is read when the pass is recorded.
  Microsoft::WRL::ComPtr<ID3D12RootSignature> depth_resolve_root;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> depth_resolve_single, depth_resolve_ms, sample_resolve;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> depth_resolve_rtv;
  uint64_t depth_resolves = 0, sample_resolves = 0;
  struct Stage {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    uint32_t width = 0, height = 0, key = 0;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
    uint64_t last_use_fence = 0;
    bool staged = false, presented = false;
  };
  std::array<Stage, kStageSlots> stages;
  std::array<uint32_t, 768> gamma{};  // 10-bit values; identity until the title sets a ramp.
  bool gamma_from_title = false, vsync = true, allow_tearing = true;
  uint32_t swap_buffers = 2, max_frame_latency = 1;  // The PresentMode of the attach.
  PresentMode mode;  // Brightness and output effects of the attach.
  // Each 10-bit ramp value with the brightness exponent; used when it is not 0.5.
  std::array<uint16_t, 1024> brightness_ramp{};
  bool brightness = false;
  uint64_t staged_frames = 0, presents = 0, presents_skipped_empty = 0, resizes = 0, gamma_updates = 0;
  uint64_t latency_waits = 0, latency_timeouts = 0, captures = 0;
  bool BackBufferViews(HRESULT& hr);  // RTVs of the current back buffers.
  // The UI layer of the Present (UiFrame): RGBA8 textures by key, each with
  // an SRV slot in ui_views (key 0 the white texture), the frame of SetUiFrame
  // and the root signature and pipeline of ui_pass.hlsl. A replaced or
  // released texture and its slot wait for the fence of their last use.
  struct UiTexture {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    uint32_t width = 0, height = 0;
    uint32_t view = UINT32_MAX;  // Its slot in ui_views; UINT32_MAX = none.
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
    uint64_t last_use_fence = 0;
  };
  static constexpr uint32_t kUiViews = 256;
  std::unordered_map<uint32_t, UiTexture> ui_textures;
  std::vector<UiTexture> retired_ui_textures;
  std::vector<uint32_t> free_ui_views;
  uint32_t next_ui_view = 0;
  UiFrame ui_frame;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> ui_views;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> ui_root;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> ui_pipeline;
  // Draws whose texture was released after their frame was set are skipped.
  uint64_t ui_texture_uploads = 0, ui_frames_drawn = 0, ui_draws_skipped = 0;
  bool UiFrameVisible() const {
    return ui_frame.width > 0 && ui_frame.height > 0 && !ui_frame.draws.empty() && !ui_frame.indices.empty();
  }
  // Root signature, pipeline, heap and the white texture, once; in the open batch.
  bool EnsureUiLayer(const char*& reason, HRESULT& hr);
  // Creates (or recreates at a new size) and fills the texture of key in the open batch.
  bool WriteUiTexture(uint32_t key, uint32_t width, uint32_t height, const uint8_t* rgba, const char*& reason,
                      HRESULT& hr);
  // The frame's draws into the back buffer of rtv, in the open batch.
  bool DrawUi(ID3D12GraphicsCommandList* commands, D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t width, uint32_t height,
              const char*& reason, HRESULT& hr);
};
namespace {
constexpr size_t kUploadPageBytes = size_t(8) << 20;  // Pooled page; larger requests get their own.
// The state a buffer of the draw layer rests in between uploads.
constexpr D3D12_RESOURCE_STATES kBufferRead =
    D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER;
constexpr uint64_t kBatchOperationsMax = 2048;         // A longer batch closes without waiting.
}  // namespace

// Batch helpers, owner thread only. A batch returns its allocator, its
// pooled upload pages and its retired view slots once its fence completed.
void Backend::State::Reclaim(uint64_t completed) {
  if (completed == UINT64_MAX) return;
  while (!batches_in_flight.empty() && batches_in_flight.front().fence <= completed) {
    Batch& done = batches_in_flight.front();
    if (SUCCEEDED(done.allocator->Reset())) free_allocators.push_back(std::move(done.allocator));
    for (auto& page : done.pages)
      if (page.size == kUploadPageBytes) {
        page.used = 0;
        free_pages.push_back(std::move(page));
      }
    for (uint32_t view : done.views) free_views.push_back(view);
    batches_in_flight.pop_front();
  }
  ReleaseRetiredTextures(completed);
}

bool Backend::State::OpenBatch(const char*& reason, HRESULT& hr) {
  if (batch_open) return true;
  auto* d3d = static_cast<plume::D3D12Device*>(device.get())->d3d;
  auto* f = static_cast<plume::D3D12CommandFence*>(fence.get());
  Reclaim(f->d3d->GetCompletedValue());
  Batch next;
  next.fence = f->fenceValue;
  hr = E_FAIL;
  if (!next.fence || next.fence == UINT64_MAX) { reason = "native-batch-fence-exhausted"; return false; }
  if (!free_allocators.empty()) {
    next.allocator = std::move(free_allocators.back());
    free_allocators.pop_back();
  } else {
    hr = d3d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&next.allocator));
    if (FAILED(hr)) { reason = "native-batch-allocator"; return false; }
    ++allocators_created;
  }
  if (!batch_list) {
    hr = d3d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, next.allocator.Get(), nullptr,
                                IID_PPV_ARGS(&batch_list));
    if (FAILED(hr)) { reason = "native-batch-list"; return false; }
  } else if (FAILED(hr = batch_list->Reset(next.allocator.Get(), nullptr))) {
    reason = "native-batch-list-reset";
    return false;
  }
  hr = S_OK;
  batch = std::move(next);
  batch_open = true;
  return true;
}

// One ExecuteCommandLists and one Signal for everything recorded since the open.
bool Backend::State::CloseBatch(const char*& reason, HRESULT& hr) {
  hr = S_OK;
  if (!batch_open) return true;
  // After a failure the open batch is never closed nor submitted.
  if (failures->Failed()) {
    reason = "native-batch-refused-after-failure";
    hr = E_ABORT;
    return false;
  }
  profile::Timer submit_time(profile::kEngineSubmit);
  auto* f = static_cast<plume::D3D12CommandFence*>(fence.get());
  batch_open = false;
  if (FAILED(hr = batch_list->Close())) { operation_poisoned = true; reason = "native-batch-close"; return false; }
  if (batch.fence != f->fenceValue) {
    operation_poisoned = true;
    reason = "native-batch-fence-moved";
    hr = E_FAIL;
    return false;
  }
  if (!Submit(batch_list.Get(), batch.fence, hr)) {
    batch_refused = hr == E_ABORT;
    reason = batch_refused ? "native-batch-refused-after-failure" : "native-batch-signal";
    return false;
  }
  pending_signal = batch.fence;
  ++f->fenceValue;
  ++batches_closed;
  profile::Count(profile::kSubmissions, 1);
  batches_in_flight.push_back(std::move(batch));
  batch = {};
  archiveClearWarnings(info.Get(), !surfaces.empty(), archived_clear_warnings);
  return true;
}

bool Backend::State::Submit(ID3D12GraphicsCommandList* list, uint64_t fence_value, HRESULT& hr) {
  auto* q = static_cast<plume::D3D12CommandQueue*>(queue.get());
  auto* f = static_cast<plume::D3D12CommandFence*>(fence.get());
  hr = E_ABORT;
  const bool authorized = failures->Authorize([&] {
    ID3D12CommandList* lists[] = {list};
    unproved_execution = true;
    q->d3d->ExecuteCommandLists(1, lists);
    hr = q->d3d->Signal(f->d3d, fence_value);
  });
  if (!authorized) return false;
  if (FAILED(hr)) {
    operation_poisoned = true;
    return false;
  }
  unproved_execution = false;
  return true;
}

// Upload memory of the open batch: pooled persistent pages, mapped once,
// 512-byte aligned (texture placement; also the 256 of root CBVs).
bool Backend::State::Upload(size_t bytes, UploadPage*& page, size_t& offset, HRESULT& hr) {
  constexpr size_t kAlign = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
  hr = S_OK;
  if (!batch.pages.empty()) {
    auto& current = batch.pages.back();
    const size_t at = (current.used + kAlign - 1) & ~(kAlign - 1);
    if (at + bytes <= current.size) {
      current.used = at + bytes;
      page = &current;
      offset = at;
      return true;
    }
  }
  UploadPage next;
  if (bytes <= kUploadPageBytes && !free_pages.empty()) {
    next = std::move(free_pages.back());
    free_pages.pop_back();
  } else {
    next.size = bytes <= kUploadPageBytes ? kUploadPageBytes : (bytes + 0xFFFF) & ~size_t(0xFFFF);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = next.size;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    auto* d3d = static_cast<plume::D3D12Device*>(device.get())->d3d;
    hr = d3d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                      IID_PPV_ARGS(&next.buffer));
    if (FAILED(hr)) return false;
    const D3D12_RANGE none{0, 0};
    if (FAILED(hr = next.buffer->Map(0, &none, reinterpret_cast<void**>(&next.cpu)))) return false;
    next.gpu = next.buffer->GetGPUVirtualAddress();
    ++upload_pages_created;
  }
  next.used = bytes;
  batch.pages.push_back(std::move(next));
  page = &batch.pages.back();
  offset = 0;
  return true;
}

Backend::Backend(std::shared_ptr<FailureChannel> failures) : state_(std::make_unique<State>()) {
  state_->failures = std::move(failures);
}
Backend::~Backend() {
  std::string evidence;
  // A caller must abort if draining failed. Never implicitly destroy resources
  // referenced by work whose completion has not been proved.
  if (!Shutdown(evidence)) ExitUnreleased("native-d3d12-backend-destructor", evidence);
}

bool Backend::Initialize(std::string& evidence, const RenderMode& mode) {
  auto fail = [&](const char* operation, HRESULT result = E_FAIL) {
    evidence = fmt::format("{{\"operation\":\"{}\",\"hresult\":{},\"hresult_failed\":{},"
                           "\"readiness_criteria_met\":false}}", operation,
                           static_cast<uint32_t>(result), FAILED(result));
    return false;
  };
  if (state_->render) return fail("duplicate-native-initialization");
  if (debugLayerOption() < 0) return fail("native-debug-layer-option");
  if (const char* reason = RenderModeError(mode)) return fail(reason);
  const ResolutionScale& scale = mode.scale;
  // Enable before Plume creates any device. Release our debug COM reference.
  HRESULT result = S_OK;
  if (debugLayerOption()) {
    Microsoft::WRL::ComPtr<ID3D12Debug> debug;
    result = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
    if (FAILED(result)) return fail("D3D12GetDebugInterface", result);
    debug->EnableDebugLayer();
  }
  state_->render = plume::CreateD3D12Interface();
  if (!state_->render) return fail("Plume-interface");
  state_->device = state_->render->createDevice(mode.device);
  if (!state_->device) return fail("Plume-device");
  auto* device = static_cast<plume::D3D12Device*>(state_->device.get());
  if (!device->d3d || !device->adapter) return fail("D3D12-device-adapter");
  if (debugLayerOption()) {
    result = device->d3d->QueryInterface(IID_PPV_ARGS(&state_->info));
    if (FAILED(result)) return fail("D3D12-info-queue", result);
  }
  // The shaders are vs_6_0 and ps_6_0 without optional features. A runtime
  // that does not know a newer model (Windows 10 stops at 6.5) answers
  // E_INVALIDARG, so the query steps down to one it knows.
  D3D12_FEATURE_DATA_SHADER_MODEL model{};
  for (auto highest : {D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3,
                       D3D_SHADER_MODEL_6_2, D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0}) {
    model.HighestShaderModel = highest;
    result = device->d3d->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model));
    if (result != E_INVALIDARG) break;
  }
  if (FAILED(result) || model.HighestShaderModel < D3D_SHADER_MODEL_6_0)
    return fail("Shader-model-6.0", result);
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  result = device->d3d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
  if (FAILED(result)) return fail("D3D12-options", result);
  // The texture and sampler tables of the draw root signature are unbounded
  // (EnsureDrawLayer).
  if (options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2) return fail("Resource-binding-tier-2");
  for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8_UNORM}) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
    result = device->d3d->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    const auto required = format == DXGI_FORMAT_R8G8B8A8_UNORM ?
        D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_BLENDABLE :
        D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE;
    if (FAILED(result) || (support.Support1 & required) != required)
      return fail("native-format-support", result);
  }
  // The MSAA sample count, lowered to one the device supports for both
  // surface formats.
  state_->render_mode = mode;
  auto msaa_supported = [&](uint32_t samples) {
    for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D32_FLOAT_S8X24_UINT}) {
      D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{format, samples};
      if (FAILED(device->d3d->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels,
                                                  sizeof(levels))) || !levels.NumQualityLevels) return false;
    }
    return true;
  };
  state_->render_mode.msaa_samples = SupportedSamples(mode.msaa_samples, msaa_supported);
  state_->pipeline_msaa.store(state_->render_mode.msaa_samples);
  state_->queue = state_->device->createCommandQueue(plume::RenderCommandListType::DIRECT);
  state_->fence = state_->device->createCommandFence();
  if (!state_->queue || !state_->fence) return fail("Plume-queue-fence");
  auto* queue = static_cast<plume::D3D12CommandQueue*>(state_->queue.get());
  auto* fence = static_cast<plume::D3D12CommandFence*>(state_->fence.get());
  if (!queue->d3d || !fence->d3d || !fence->fenceEvent) return fail("native-queue-fence-handles");
  // Only host readiness: a GPU queue signal with a real completion event.
  // This is never published as a guest fence, read pointer or draw completion.
  const auto signal = fence->fenceValue;
  result = queue->d3d->Signal(fence->d3d, signal);
  if (FAILED(result)) return fail("native-queue-signal", result);
  state_->pending_signal = signal;
  result = fence->d3d->SetEventOnCompletion(signal, fence->fenceEvent);
  if (FAILED(result)) return fail("native-fence-event", result);
  const DWORD wait = WaitForSingleObject(fence->fenceEvent, 5000);
  if (wait != WAIT_OBJECT_0)
    return fail("native-readiness-wait", HRESULT_FROM_WIN32(
        wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : wait == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE));
  state_->completed = fence->d3d->GetCompletedValue();
  if (state_->completed == UINT64_MAX || state_->completed < signal)
    return fail("native-fence-incomplete");
  fence->fenceValue = signal + 1;
  result = device->d3d->GetDeviceRemovedReason();
  if (FAILED(result)) return fail("native-device-removed", result);
  state_->pending_signal = 0;
  DXGI_ADAPTER_DESC1 adapter{};
  result = device->adapter->GetDesc1(&adapter);
  if (SUCCEEDED(result) && adapter.VendorId == 0x10DE) state_->shader_read |= D3D12_RESOURCE_STATE_COPY_SOURCE;
  if (FAILED(result)) return fail("native-adapter-description", result);
  if (state_->info && state_->info->GetNumStoredMessagesAllowedByRetrievalFilter() != 0)
    return fail("native-debug-messages");
  // Adapter name and user-mode driver version of every execution (image
  // comparisons hold for one adapter and driver).
  std::string adapter_name;
  for (const wchar_t* c = adapter.Description; *c && adapter_name.size() < 256; ++c) {
    if (*c < 32 || *c > 126) { adapter_name += '?'; continue; }
    if (*c == L'"' || *c == L'\\') adapter_name += '\\';
    adapter_name += char(*c);
  }
  LARGE_INTEGER umd{};
  const HRESULT umd_result = device->adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd);
  const std::string driver = SUCCEEDED(umd_result) ?
      fmt::format("\"{}.{}.{}.{}\"", uint32_t(umd.HighPart) >> 16, uint32_t(umd.HighPart) & 0xFFFF,
                  uint32_t(umd.LowPart) >> 16, uint32_t(umd.LowPart) & 0xFFFF) : std::string("null");
  // The adapter asked by name, when one was.
  std::string requested;
  if (!mode.device.empty()) {
    std::string name;
    for (char c : mode.device.substr(0, 256)) {
      if (static_cast<unsigned char>(c) < 32) { name += '?'; continue; }
      if (c == '"' || c == '\\') name += '\\';
      name += c;
    }
    requested = fmt::format(",\"device_requested\":\"{}\",\"device_matched\":{}", name,
                            state_->device->getDescription().name == mode.device);
  }
  for (uint32_t i = 0; i < 256; ++i)
    state_->gamma[i] = state_->gamma[256 + i] = state_->gamma[512 + i] = (i * 1023 + 127) / 255;
  state_->epoch = AllocateEngineEpoch();
  if (state_->epoch == 0) return fail("native-owner-epoch-exhausted");
  state_->owner_thread = GetCurrentThreadId();
  state_->ready = true;
  evidence = fmt::format("{{\"backend\":\"Plume/D3D12\",\"device_vendor\":{},\"device_id\":{},"
      "\"adapter_luid_low\":{},\"adapter_luid_high\":{},\"adapter_description\":\"{}\",\"adapter_revision\":{},"
      "\"adapter_subsys\":{},\"driver_umd_version\":{},\"driver_umd_hresult\":{},\"shader_model\":{},"
      "\"binding_tier\":{},"
      "\"rgba8_render_blend\":true,\"r8_sample\":true,\"direct_queue\":true,\"fence_signal\":{},"
      "\"fence_completed\":{},\"debug_messages\":0,\"debug_layer\":{},\"guest_completion\":false,\"presentation\":false{}{}}}",
      adapter.VendorId, adapter.DeviceId, adapter.AdapterLuid.LowPart, adapter.AdapterLuid.HighPart, adapter_name,
      adapter.Revision, adapter.SubSysId, driver, static_cast<uint32_t>(umd_result),
      static_cast<uint32_t>(model.HighestShaderModel), static_cast<uint32_t>(options.ResourceBindingTier),
      signal, state_->completed, debugLayerOption() != 0,
      // Only a mode other than the default is recorded.
      scale.Unit() && !mode.msaa_samples ? std::string() : fmt::format(
          ",\"render_mode\":{{\"scale\":[{},{}],\"msaa_samples\":{},\"msaa_requested\":{}}}", scale.x, scale.y,
          state_->render_mode.msaa_samples, mode.msaa_samples), requested);
  return true;
}
std::string Backend::DeviceName() const {
  return state_->device ? state_->device->getDescription().name : std::string();
}
bool Backend::Shutdown(std::string& evidence) {
  if (state_->frontend.active()) {
    evidence = "{\"released\":false,\"reason\":\"frontend-session-still-retained\"}";
    return false;
  }
  if (state_->ready && state_->owner_thread != GetCurrentThreadId()) {
    evidence = "{\"released\":false,\"reason\":\"native-owner-thread\",\"guest_shutdown\":false}";
    return false;
  }
  if (state_->unproved_execution) {
    evidence = "{\"released\":false,\"reason\":\"executed-work-without-completion-marker\","
               "\"guest_shutdown\":false}";
    return false;
  }
  state_->pipeline_set.Stop();  // The asynchronous pipeline threads end before anything is released.
  // Signal may succeed while SetEvent/wait subsequently fails. Re-check the
  // actual fence before release; if unproved, keep ownership for diagnostic
  // process termination. No destruction or guest completion is then claimed.
  if (state_->pending_signal) {
    const auto* fence = static_cast<plume::D3D12CommandFence*>(state_->fence.get());
    const auto* device = static_cast<plume::D3D12Device*>(state_->device.get());
    const uint64_t completed = fence && fence->d3d ? fence->d3d->GetCompletedValue() : 0;
    const HRESULT removed = device && device->d3d ? device->d3d->GetDeviceRemovedReason() : E_FAIL;
    if (completed == UINT64_MAX || completed < state_->pending_signal || FAILED(removed)) {
      evidence = fmt::format("{{\"released\":false,\"pending_signal\":{},\"observed_completed\":{},"
          "\"device_removed_reason\":{},\"reason\":\"native-completion-unproved-abort-required\","
          "\"guest_shutdown\":false}}", state_->pending_signal, completed, static_cast<uint32_t>(removed));
      return false;
    }
    state_->completed = completed;
    state_->pending_signal = 0;
  }
  const auto debug = state_->info ? colorDebugStatus(state_->info.Get(), !state_->surfaces.empty()) : ColorDebugStatus{};
  const auto archive = state_->info ? debugMessageIds(state_->info.Get()) : DebugArchive{"[]", 0, true};
  const bool archiveComplete = archive.complete && archive.copied == debug.total;
  const auto* device = static_cast<plume::D3D12Device*>(state_->device.get());
  const HRESULT removed = device && device->d3d ? device->d3d->GetDeviceRemovedReason() : S_OK;
  // The device infoqueue may survive across device creations. Archive the
  // completed owner's accepted diagnostics before clearing them. Unexpected
  // messages are preserved and keep this teardown rejected.
  const bool archiveAndClear = state_->info && archiveComplete && debug.unexpected == 0 && SUCCEEDED(removed);
  if (archiveAndClear) state_->info->ClearStoredMessages();
  state_->retired_surfaces.clear();  // pending_signal completion was proved above.
  state_->retired_textures.clear();
  state_->retired_sampled.clear();
  state_->retired_buffers.clear();
  state_->submissions.clear();
  // An open batch was never submitted; it is discarded, not claimed complete.
  const bool open_batch_discarded = state_->batch_open || state_->batch_refused;
  const uint64_t open_batch_operations = open_batch_discarded ? state_->batch.operations : 0;
  state_->batch_refused = false;
  state_->batch = {};
  state_->batch_open = false;
  state_->batches_in_flight.clear();
  state_->free_allocators.clear();
  state_->free_pages.clear();
  state_->batch_list.Reset();
  state_->pipelines.clear();
  state_->pipeline_records.clear();
  state_->pipeline_set.Clear();  // The pool was joined before.
  state_->vs_block.fill(0);
  state_->ps_block.fill(0);
  state_->cbv_batch_fence = state_->last_vs_version = state_->last_ps_version = 0;
  state_->last_vs_cbv = state_->last_ps_cbv = state_->last_shared_cbv = 0;
  state_->sampled.clear();
  state_->buffers.clear();
  state_->buffer_heaps.clear();
  state_->sampler_slots.clear();
  state_->sampler_slot_keys.clear();
  state_->sampler_slot_use.clear();
  state_->free_views.clear();
  state_->next_view = 1;
  state_->draw_samplers.Reset();
  state_->draw_views.Reset();
  state_->draw_root.Reset();
  // The swapchain and its views; their last use completed above.
  const bool swapchain_released = state_->swapchain != nullptr;
  state_->present_rtv.Reset();
  state_->swapchain.reset();
  state_->window = nullptr;
  for (auto& stage : state_->stages) stage = {};
  state_->ReleaseEffects();
  state_->effect_pipelines.clear();
  state_->effect_root.Reset();
  state_->fxaa_root.Reset();
  state_->effect_rtv.Reset();
  state_->present_views.Reset();
  state_->present_pipeline.Reset();
  state_->present_root.Reset();
  state_->ui_textures.clear();  // Their last use completed above.
  state_->retired_ui_textures.clear();
  state_->free_ui_views.clear();
  state_->next_ui_view = 0;
  state_->ui_frame = {};
  state_->ui_views.Reset();
  state_->ui_pipeline.Reset();
  state_->ui_root.Reset();
  state_->depth_resolve_rtv.Reset();
  state_->sample_resolve.Reset();
  state_->depth_resolve_ms.Reset();
  state_->depth_resolve_single.Reset();
  state_->depth_resolve_root.Reset();
  state_->textures.clear();
  state_->surfaces.clear();
  state_->ready = false;
  state_->operation_poisoned = false;
  state_->owner_thread = 0;
  state_->epoch = 0;
  state_->info.Reset();
  state_->fence.reset();
  state_->queue.reset();
  state_->device.reset();
  state_->render.reset();
  const auto failure = state_->failures->First();
  evidence = fmt::format("{{\"fence_completed\":{},\"debug_messages\":{},\"expected_clear_warnings\":{},"
      "\"unexpected_debug_messages\":{},\"debug_message_ids\":{},\"accepted_diagnostics_cleared\":{},"
      "\"debug_archive_complete\":{},\"device_removed_reason\":{},\"archived_clear_warnings\":{},\"debug_layer\":{},"
      "\"batches_closed\":{},\"open_batch_discarded\":{},\"open_batch_operations\":{},\"allocators\":{},"
      "\"upload_pages\":{},\"posted_failure_sequence\":{},\"swapchain_released\":{},\"presents\":{},"
      "\"released\":[\"info-queue\",\"fence-event-and-fence\",\"queue\",\"device\",\"interface\"],"
      "\"guest_shutdown\":false}}", state_->completed, debug.total, debug.expected_clear_warnings,
      debug.unexpected, archive.ids, archiveAndClear, archiveComplete, static_cast<uint32_t>(removed),
      state_->archived_clear_warnings, debugLayerOption() != 0, state_->batches_closed, open_batch_discarded,
      open_batch_operations, state_->allocators_created, state_->upload_pages_created,
      failure ? failure->sequence : 0, swapchain_released, state_->presents);
  return archiveComplete && debug.unexpected == 0 && SUCCEEDED(removed);
}

bool Backend::InitializeFrontend(uint64_t generation, std::string& evidence) {
  // No admitted operation, resource generation or GPU callback is discarded.
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId() ||
      state_->operation_poisoned || state_->pending_signal || state_->unproved_execution ||
      !state_->frontend.Initialize(generation)) {
    evidence = "{\"reason\":\"frontend-not-cold-or-duplicate\"}";
    return false;
  }
  return VerifyFrontend(generation, evidence);
}

bool Backend::VerifyFrontend(uint64_t generation, std::string& evidence) {
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId() || !state_->device) {
    evidence = "{\"reason\":\"frontend-generation-idle-or-device-unproved\",\"failed\":[\"owner\"]}";
    return false;
  }
  // Each failed condition and the stored debug messages are named.
  const auto debug = colorDebugStatus(state_->info.Get(), !state_->surfaces.empty());
  const HRESULT removed = static_cast<plume::D3D12Device*>(state_->device.get())->d3d->GetDeviceRemovedReason();
  std::string failed;
  auto check = [&](bool ok, const char* name) {
    if (!ok) failed += fmt::format("{}\"{}\"", failed.empty() ? "" : ",", name);
  };
  check(state_->frontend.Matches(generation) && !state_->frontend.CanConsume(), "frontend-generation");
  check(!state_->operation_poisoned, "operation-poisoned");
  check(!state_->pending_signal, "pending-signal");
  check(!state_->unproved_execution, "unproved-execution");
  check(debug.unexpected == 0, "debug-messages");
  check(SUCCEEDED(removed), "device-removed");
  if (!failed.empty()) {
    const auto archive = state_->info ? debugMessageIds(state_->info.Get()) : DebugArchive{"[]", 0, true};
    evidence = fmt::format("{{\"reason\":\"frontend-generation-idle-or-device-unproved\",\"failed\":[{}],"
        "\"debug_messages\":{},\"expected_clear_warnings\":{},\"unexpected_debug_messages\":{},"
        "\"debug_message_ids\":{},\"archived_clear_warnings\":{},\"pending_signal\":{},\"device_removed_reason\":{}}}",
        failed, debug.total, debug.expected_clear_warnings, debug.unexpected, archive.ids,
        state_->archived_clear_warnings, state_->pending_signal, static_cast<uint32_t>(removed));
    return false;
  }
  evidence = FrontendReadyEvidence(generation, state_->epoch, GetCurrentThreadId());
  return true;
}

bool Backend::ReleaseFrontend(uint64_t generation, std::string& evidence) {
  if (!VerifyFrontend(generation, evidence) || !state_->frontend.Release(generation)) return false;
  evidence = FrontendReleasedEvidence(generation);
  return true;
}

bool Backend::CreateSurface(const SurfaceDesc& guest, std::string& evidence) {
  const SurfaceDesc desc = state_->render_mode.Host(guest);
  auto fail = [&](const char* reason) {
    evidence = fmt::format("{{\"created\":false,\"reason\":\"{}\",\"key\":{}}}", reason, desc.key);
    return false;
  };
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId() || state_->operation_poisoned)
    return fail("native-surface-owner-or-state");
  if (!desc.key || !desc.width || !desc.height || desc.width > 8192 || desc.height > 8192)
    return fail("native-surface-description");
  if (state_->surfaces.count(desc.key)) return fail("native-surface-key-live");
  const bool depth = desc.format == SurfaceFormat::DepthStencil;
  if (desc.samples != 1 && desc.samples != 2 && desc.samples != 4 && desc.samples != 8)
    return fail("native-surface-samples");
  const plume::RenderMultisampling samples(desc.samples == 8 ? plume::RenderSampleCount::COUNT_8 :
      desc.samples == 4 ? plume::RenderSampleCount::COUNT_4 :
      desc.samples == 2 ? plume::RenderSampleCount::COUNT_2 : plume::RenderSampleCount::COUNT_1);
  auto texture = state_->device->createTexture(depth ?
      plume::RenderTextureDesc::DepthTarget(desc.width, desc.height, plume::RenderFormat::D32_FLOAT_S8_UINT, samples) :
      plume::RenderTextureDesc::ColorTarget(desc.width, desc.height, plume::RenderFormat::R8G8B8A8_UNORM, samples));
  auto* native = static_cast<plume::D3D12Texture*>(texture.get());
  if (!texture || !native->d3d) return fail("native-surface-create");
  const auto d = native->d3d->GetDesc();
  if (d.Width != desc.width || d.Height != desc.height || d.SampleDesc.Count != desc.samples)
    return fail("native-surface-created-description");
  auto* device = static_cast<plume::D3D12Device*>(state_->device.get());
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> view;
  D3D12_DESCRIPTOR_HEAP_DESC heap{depth ? D3D12_DESCRIPTOR_HEAP_TYPE_DSV : D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1,
                                  D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  if (FAILED(device->d3d->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&view)))) return fail("native-surface-view-heap");
  if (depth) {
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dsv.ViewDimension = desc.samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
    device->d3d->CreateDepthStencilView(native->d3d, &dsv, view->GetCPUDescriptorHandleForHeapStart());
  } else {
    device->d3d->CreateRenderTargetView(native->d3d, nullptr, view->GetCPUDescriptorHandleForHeapStart());
  }
  State::Surface surface{desc, std::move(texture), std::move(view), native->resourceStates, 0, guest};
  state_->surfaces.emplace(desc.key, std::move(surface));
  evidence = fmt::format("{{\"created\":true,\"key\":{},\"width\":{},\"height\":{},\"guest_format\":{},"
      "\"host_format\":\"{}\",\"samples\":{},\"live_surfaces\":{}}}", desc.key, desc.width, desc.height,
      desc.guest_format, depth ? "D32_FLOAT_S8_UINT" : "R8G8B8A8_UNORM", desc.samples, state_->surfaces.size());
  return true;
}

bool Backend::Reconfigure(const RenderMode& requested, const PresentMode& present, std::string& evidence) {
  RenderMode render = requested;
  auto& s = *state_;
  auto fail = [&](const char* reason) {
    evidence = fmt::format("{{\"applied\":false,\"reason\":\"{}\"}}", reason); return false;
  };
  if (!s.ready || s.owner_thread != GetCurrentThreadId() || s.operation_poisoned || s.unproved_execution)
    return fail("native-reconfigure-owner-or-state");
  if (!ValidLiveModes(s.render_mode, render, present)) return fail("native-reconfigure-mode");
  auto* device = static_cast<plume::D3D12Device*>(s.device.get());
  render.msaa_samples = SupportedSamples(requested.msaa_samples, [&](uint32_t samples) {
    for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D32_FLOAT_S8X24_UINT}) {
      D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{format, samples};
      if (FAILED(device->d3d->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels, sizeof(levels))) ||
          !levels.NumQualityLevels) return false;
    }
    return true;
  });
  const bool targets = render.scale.x != s.render_mode.scale.x || render.scale.y != s.render_mode.scale.y ||
                       render.msaa_samples != s.render_mode.msaa_samples;
  for (const auto& [key, surface] : s.surfaces) {
    const auto desc = render.Host(surface.guest);
    if (!desc.width || !desc.height || desc.width > 8192 || desc.height > 8192)
      return fail("native-reconfigure-surface-size");
  }
  if (!WaitIdle(evidence)) return false;
  const auto old_render = s.render_mode;
  const auto old_present = s.mode;
  decltype(s.surfaces) old_surfaces;
  if (targets) {
    old_surfaces.swap(s.surfaces);
    s.render_mode = render;
    for (const auto& [key, surface] : old_surfaces) if (!CreateSurface(surface.guest, evidence)) {
      s.surfaces.clear(); s.surfaces.swap(old_surfaces); s.render_mode = old_render;
      return false;
    }
  }
  const bool swap = s.swapchain && (present.buffers != old_present.buffers ||
      present.max_frame_latency != old_present.max_frame_latency || present.allow_tearing != old_present.allow_tearing);
  bool applied = true;
  if (swap) {
    const auto window = platform::WindowHandles(s.window);
    s.present_rtv.Reset(); s.swapchain.reset();
    applied = AttachWindow(window, evidence, present);
    if (!applied) {
      const auto error = evidence;
      std::string rollback;
      if (!AttachWindow(window, rollback, old_present)) {
        // A failed restore cannot leave the game submitting to a dead display.
        s.operation_poisoned = true;
      }
      evidence = error;
    }
  } else {
    s.mode = present; s.allow_tearing = present.allow_tearing;
    s.swap_buffers = present.buffers; s.max_frame_latency = present.max_frame_latency;
    s.brightness = std::fabs(present.brightness - 0.5f) > 0.001f;
    const double exponent = 1.0 / std::clamp(1.0 + (double(present.brightness) - 0.5) * 1.2, 0.1, 4.0);
    for (uint32_t i = 0; i < 1024; ++i)
      s.brightness_ramp[i] = uint16_t(std::lround(1023.0 * std::pow(i / 1023.0, exponent)));
    applied = SetVsync(present.vsync, evidence);
  }
  if (!applied) {
    if (targets) { s.surfaces.clear(); s.surfaces.swap(old_surfaces); s.render_mode = old_render; }
    return false;
  }
  s.render_mode = render;
  s.pipeline_msaa.store(render.msaa_samples);
  evidence = fmt::format("{{\"applied\":true,\"targets_rebuilt\":{},\"surfaces\":{},\"resolved_textures_retained\":{},"
      "\"scale\":[{},{}],\"msaa_samples\":{},\"vsync\":{},\"brightness\":{},\"buffers\":{},\"latency\":{},"
      "\"effect\":{},\"post_effect\":{}}}", targets, s.surfaces.size(), s.textures.size(), render.scale.x,
      render.scale.y, render.msaa_samples, present.vsync, present.brightness, present.buffers, present.max_frame_latency,
      uint32_t(present.effect), uint32_t(present.post_effect));
  return true;
}

bool Backend::EnsureSurface(const SurfaceDesc& desc, std::string& evidence) {
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId()) {
    evidence = "{\"ensured\":false,\"reason\":\"native-surface-owner\"}";
    return false;
  }
  auto it = state_->surfaces.find(desc.key);
  if (it != state_->surfaces.end()) {
    const auto& d = it->second.desc;
    const SurfaceDesc host = state_->render_mode.Host(desc);
    if (d.width == host.width && d.height == host.height && d.format == host.format && d.samples == host.samples) {
      evidence = fmt::format("{{\"ensured\":true,\"created\":false,\"key\":{}}}", desc.key);
      return true;
    }
    std::string destroyed;
    if (!DestroySurface(desc.key, destroyed)) { evidence = destroyed; return false; }
  }
  return CreateSurface(desc, evidence);
}

bool Backend::DestroySurface(uint32_t key, std::string& evidence) {
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId()) {
    evidence = "{\"destroyed\":false,\"reason\":\"native-surface-owner\"}";
    return false;
  }
  auto it = state_->surfaces.find(key);
  if (it == state_->surfaces.end()) {
    evidence = fmt::format("{{\"destroyed\":false,\"reason\":\"native-surface-unknown\",\"key\":{}}}", key);
    return false;
  }
  // A surface still referenced by submitted work is retired, not destroyed:
  // it is released once its fence completes (checked here and at Shutdown).
  auto* fence = static_cast<plume::D3D12CommandFence*>(state_->fence.get());
  const uint64_t completed = fence->d3d->GetCompletedValue();
  const bool in_flight = it->second.last_use_fence &&
      (completed == UINT64_MAX || completed < it->second.last_use_fence);
  const uint64_t last_use = it->second.last_use_fence;
  if (in_flight) state_->retired_surfaces.push_back(std::move(it->second));
  state_->surfaces.erase(it);
  std::erase_if(state_->retired_surfaces, [&](const State::Surface& s) {
    return completed != UINT64_MAX && completed >= s.last_use_fence; });
  evidence = fmt::format("{{\"destroyed\":true,\"key\":{},\"deferred\":{},\"last_use_fence\":{},"
      "\"observed_completed\":{},\"live_surfaces\":{},\"retired_surfaces\":{}}}", key, in_flight, last_use,
      completed, state_->surfaces.size(), state_->retired_surfaces.size());
  return true;
}

bool Backend::ClearSurfaces(const ClearRequest& request, uint64_t& fenceValue,
                                         std::string& evidence) {
  profile::Timer profile(profile::kEngineClear);
  fenceValue = 0;
  auto fail = [&](const char* reason, HRESULT hr = E_FAIL) {
    evidence = fmt::format("{{\"cleared\":false,\"reason\":\"{}\",\"hresult\":{}}}", reason, static_cast<uint32_t>(hr));
    return false;
  };
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId() || state_->operation_poisoned ||
      state_->unproved_execution) return fail("native-clear-owner-or-state");
  std::vector<State::Surface*> colors;
  for (uint32_t key : request.color_keys) {
    if (!key || !request.clear_color) continue;
    auto it = state_->surfaces.find(key);
    if (it == state_->surfaces.end() || it->second.desc.format != SurfaceFormat::Color8888)
      return fail("native-clear-color-surface-unknown");
    colors.push_back(&it->second);
  }
  State::Surface* depth = nullptr;
  if (request.depth_key && (request.clear_depth || request.clear_stencil)) {
    auto it = state_->surfaces.find(request.depth_key);
    if (it == state_->surfaces.end() || it->second.desc.format != SurfaceFormat::DepthStencil)
      return fail("native-clear-depth-surface-unknown");
    depth = &it->second;
  }
  for (float channel : request.color)
    if (!std::isfinite(channel)) return fail("native-clear-color-value");
  if (!std::isfinite(request.depth)) return fail("native-clear-depth-value");
  // Recorded into the open batch, complete with its fence.
  HRESULT hr = S_OK;
  const char* batch_reason = nullptr;
  if (!state_->OpenBatch(batch_reason, hr)) return fail(batch_reason, hr);
  ID3D12GraphicsCommandList* const commands = state_->batch_list.Get();
  auto transition = [&](State::Surface& surface, D3D12_RESOURCE_STATES after) {
    if (surface.state == after) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = static_cast<plume::D3D12Texture*>(surface.texture.get())->d3d;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = surface.state;
    b.Transition.StateAfter = after;
    commands->ResourceBarrier(1, &b);
    surface.state = after;
  };
  auto clip = [&](const State::Surface& surface, D3D12_RECT& rect) {
    HostRect host;
    const bool covered = ClearRect(state_->render_mode.scale, request, surface.desc.width, surface.desc.height, host);
    rect = {host.left, host.top, host.right, host.bottom};
    return covered;
  };
  for (auto* surface : colors) {
    D3D12_RECT rect{};
    transition(*surface, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (clip(*surface, rect))
      commands->ClearRenderTargetView(surface->view->GetCPUDescriptorHandleForHeapStart(),
                                                request.color.data(), 1, &rect);
  }
  if (depth) {
    D3D12_RECT rect{};
    transition(*depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const auto flags = D3D12_CLEAR_FLAGS((request.clear_depth ? D3D12_CLEAR_FLAG_DEPTH : 0) |
                                         (request.clear_stencil ? D3D12_CLEAR_FLAG_STENCIL : 0));
    if (clip(*depth, rect))
      commands->ClearDepthStencilView(depth->view->GetCPUDescriptorHandleForHeapStart(), flags,
                                                request.depth, request.stencil, 1, &rect);
  }
  for (auto* surface : colors) surface->last_use_fence = state_->batch.fence;
  if (depth) depth->last_use_fence = state_->batch.fence;
  fenceValue = state_->batch.fence;
  ++state_->batch.operations;
  ++state_->operations;
  archiveClearWarnings(state_->info.Get(), !state_->surfaces.empty(), state_->archived_clear_warnings);
  evidence = fmt::format("{{\"cleared\":true,\"fence\":{},\"color_surfaces\":{},\"depth\":{},"
      "\"color\":[{},{},{},{}],\"z\":{},\"stencil\":{},\"rect\":[{},{},{},{}],\"operations\":{}}}",
      fenceValue, colors.size(), depth != nullptr, request.color[0], request.color[1], request.color[2],
      request.color[3], request.depth, request.stencil, request.left, request.top, request.right, request.bottom,
      state_->operations);
  return true;
}

bool Backend::WaitIdle(std::string& evidence) { return Wait(true, evidence); }
bool Backend::WaitSubmitted(std::string& evidence) { return Wait(false, evidence); }
bool Backend::Wait(bool close, std::string& evidence) {
  profile::Timer profile(profile::kEngineWaitIdle);
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId()) {
    evidence = "{\"idle\":false,\"reason\":\"native-idle-owner\"}";
    return false;
  }
  const char* batch_reason = nullptr;
  HRESULT batch_hr = S_OK;
  if (close && !state_->CloseBatch(batch_reason, batch_hr)) {
    evidence = fmt::format("{{\"idle\":false,\"reason\":\"{}\",\"hresult\":{}}}", batch_reason,
                           static_cast<uint32_t>(batch_hr));
    return false;
  }
  auto* fence = static_cast<plume::D3D12CommandFence*>(state_->fence.get());
  auto* device = static_cast<plume::D3D12Device*>(state_->device.get());
  const uint64_t target = state_->pending_signal;
  DWORD wait = WAIT_OBJECT_0;
  if (target && fence->d3d->GetCompletedValue() < target) {
    if (FAILED(fence->d3d->SetEventOnCompletion(target, fence->fenceEvent))) {
      evidence = "{\"idle\":false,\"reason\":\"native-idle-event\"}";
      return false;
    }
    wait = WaitForSingleObject(fence->fenceEvent, 5000);
  }
  const uint64_t completed = fence->d3d->GetCompletedValue();
  const HRESULT removed = device->d3d->GetDeviceRemovedReason();
  const bool ok = wait == WAIT_OBJECT_0 && completed != UINT64_MAX && completed >= target && SUCCEEDED(removed);
  if (ok) {
    state_->completed = completed;
    state_->pending_signal = 0;  // Every submitted operation is complete.
    std::erase_if(state_->submissions, [&](const State::Submission& s) { return completed >= s.fence; });
    std::erase_if(state_->retired_surfaces, [&](const State::Surface& s) { return completed >= s.last_use_fence; });
    state_->Reclaim(completed);  // Also the retired textures.
    archiveClearWarnings(state_->info.Get(), !state_->surfaces.empty(), state_->archived_clear_warnings);
  }
  evidence = fmt::format("{{\"idle\":{},\"target\":{},\"observed_completed\":{},\"wait_result\":{},"
      "\"device_removed_reason\":{},\"batches_closed\":{},\"allocators\":{},\"upload_pages\":{}}}", ok, target,
      completed, wait, static_cast<uint32_t>(removed), state_->batches_closed, state_->allocators_created,
      state_->upload_pages_created);
  return ok;
}

// Frame lot of D3DDevice_Swap. Closes the open batch without waiting and
// has the host event set when the highest Signal on the queue completes, so
// every operation submitted so far is done when it fires.
bool Backend::CloseFrame(FrameEvent event, uint64_t& target, std::string& evidence) {
  target = 0;
  auto fail = [&](const char* reason, HRESULT hr = E_FAIL) {
    evidence = fmt::format("{{\"closed\":false,\"reason\":\"{}\",\"hresult\":{}}}", reason, static_cast<uint32_t>(hr));
    return false;
  };
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId() || !event || state_->operation_poisoned ||
      state_->unproved_execution) return fail("native-frame-owner-or-state");
  const char* batch_reason = nullptr;
  HRESULT hr = S_OK;
  if (!state_->CloseBatch(batch_reason, hr)) return fail(batch_reason, hr);
  state_->pipeline_set.EndFrame();
  auto* fence = static_cast<plume::D3D12CommandFence*>(state_->fence.get());
  target = fence->fenceValue - 1;  // Every Signal takes fenceValue and then advances it.
  const uint64_t completed = fence->d3d->GetCompletedValue();
  if (completed == UINT64_MAX) return fail("native-frame-device-removed");
  if (completed >= target) {
    event->Set();
  } else if (FAILED(hr = fence->d3d->SetEventOnCompletion(target, static_cast<HANDLE>(event->native_handle())))) {
    return fail("native-frame-event-on-completion", hr);
  }
  evidence = fmt::format("{{\"closed\":true,\"target\":{},\"completed\":{},\"batches_closed\":{}}}", target,
                         completed, state_->batches_closed);
  return true;
}

// RTVs of the current back buffers (after creation and every resize).
bool Backend::State::BackBufferViews(HRESULT& hr) {
  auto* d3d = static_cast<plume::D3D12Device*>(device.get())->d3d;
  hr = S_OK;
  if (!present_rtv) {
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kMaxSwapBuffers, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(hr = d3d->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&present_rtv)))) return false;
  }
  const UINT step = d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  auto cpu = present_rtv->GetCPUDescriptorHandleForHeapStart();
  for (uint32_t i = 0; i < swap_buffers; ++i, cpu.ptr += step) {
    auto* buffer = static_cast<plume::D3D12Texture*>(swapchain->getTexture(i))->d3d;
    if (!buffer) { hr = E_FAIL; return false; }
    d3d->CreateRenderTargetView(buffer, nullptr, cpu);
  }
  return true;
}

// An effect target of the given size and format; the caller waited for
// idle when an existing one is replaced (its views are rewritten in place).
bool Backend::State::EnsureEffectTarget(uint32_t i, uint32_t width, uint32_t height, DXGI_FORMAT format, bool uav,
                                       HRESULT& hr) {
  auto& target = effect_targets[i];
  if (target.resource && target.width == width && target.height == height && target.format == format) return true;
  target = {};
  auto* d3d = static_cast<plume::D3D12Device*>(device.get())->d3d;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  if (FAILED(hr = d3d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                               IID_PPV_ARGS(&target.resource))))
    return false;
  target.width = width;
  target.height = height;
  target.format = format;
  const UINT step = d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto view = present_views->GetCPUDescriptorHandleForHeapStart();
  view.ptr += SIZE_T(step) * (kStageSlots + 2 * i);
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = format;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  d3d->CreateShaderResourceView(target.resource.Get(), &srv, view);
  if (uav) {
    view.ptr += step;
    D3D12_UNORDERED_ACCESS_VIEW_DESC unordered{};
    unordered.Format = format;
    unordered.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    d3d->CreateUnorderedAccessView(target.resource.Get(), nullptr, &unordered, view);
  } else {
    auto rtv = effect_rtv->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += SIZE_T(d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)) * i;
    d3d->CreateRenderTargetView(target.resource.Get(), nullptr, rtv);
  }
  return true;
}

// The root signatures of the SDK's presenter (source t0, rectangle b0 of the
// vertex shader, effect constants b0 of the pixel shader, linear clamp s0) and of
// its FXAA (constants b0, destination u0, source t0, linear clamp s0).
bool Backend::State::EffectRoots(HRESULT& hr) {
  if (effect_root && fxaa_root) return true;
  auto* d3d = static_cast<plume::D3D12Device*>(device.get())->d3d;
  D3D12_STATIC_SAMPLER_DESC sampler{};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxAnisotropy = 1;
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
  auto create = [&](const D3D12_ROOT_SIGNATURE_DESC& desc, Microsoft::WRL::ComPtr<ID3D12RootSignature>& root) {
    Microsoft::WRL::ComPtr<ID3DBlob> blob, error;
    return SUCCEEDED(hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) &&
           SUCCEEDED(hr = d3d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                   IID_PPV_ARGS(&root)));
  };
  D3D12_DESCRIPTOR_RANGE source{};
  source.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  source.NumDescriptors = 1;
  D3D12_DESCRIPTOR_RANGE destination{};
  destination.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  destination.NumDescriptors = 1;
  D3D12_ROOT_PARAMETER params[3]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable.NumDescriptorRanges = 1;
  params[0].DescriptorTable.pDescriptorRanges = &source;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;  // The rectangle, in NDC.
  params[1].Constants = {0, 0, 4};
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;  // The largest effect: CAS resample.
  params[2].Constants = {0, 0, 8};
  params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC desc{3, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  if (!effect_root && !create(desc, effect_root)) return false;
  // The whole union is rewritten: a table's NumDescriptorRanges would stay as
  // ShaderRegister (CS_ROOT_SIGNATURE_MISMATCH).
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;  // Size and inverse size.
  params[0].Constants = {0, 0, 4};
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &destination;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[2].DescriptorTable.NumDescriptorRanges = 1;
  params[2].DescriptorTable.pDescriptorRanges = &source;
  params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  return fxaa_root || create(desc, fxaa_root);
}

// A pipeline of the effects per shader and target format; a null vertex
// shader makes the compute pipeline of FXAA.
ID3D12PipelineState* Backend::State::EffectPipeline(const void* vs, size_t vs_size, const void* ps, size_t ps_size,
                                                   ID3D12RootSignature* root, DXGI_FORMAT format, HRESULT& hr) {
  auto& pipeline = effect_pipelines[{ps, format}];
  if (pipeline) return pipeline.Get();
  auto* d3d = static_cast<plume::D3D12Device*>(device.get())->d3d;
  if (!vs) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    desc.CS = {ps, ps_size};
    if (FAILED(hr = d3d->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)))) return nullptr;
    return pipeline.Get();
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root;
  desc.VS = {vs, vs_size};
  desc.PS = {ps, ps_size};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = format;
  desc.SampleDesc.Count = 1;
  if (FAILED(hr = d3d->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)))) return nullptr;
  return pipeline.Get();
}

// The ramp at the source size, FXAA, then the chain of the plan; the last
// step draws the image rect of the back buffer, cleared to black around it.
bool Backend::State::OutputEffects(ID3D12GraphicsCommandList* commands, uint32_t slot,
                                  D3D12_CPU_DESCRIPTOR_HANDLE back_rtv, uint32_t back_width, uint32_t back_height,
                                  uint32_t x, uint32_t y, const std::vector<OutputStep>& plan,
                                  D3D12_GPU_VIRTUAL_ADDRESS ramp, HRESULT& hr) {
  auto* d3d = static_cast<plume::D3D12Device*>(device.get())->d3d;
  const auto& stage = stages[slot];
  const bool fxaa = mode.post_effect != SwapPostEffect::kNone;
  const UINT view_step = d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const UINT rtv_step = d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  const auto view = [&](uint32_t index) {
    auto handle = present_views->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += UINT64(view_step) * index;
    return handle;
  };
  const auto target_rtv = [&](uint32_t i) {
    auto handle = effect_rtv->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += SIZE_T(rtv_step) * i;
    return handle;
  };
  const auto transition = [&](EffectTarget& target, D3D12_RESOURCE_STATES after) {
    if (target.state == after) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = target.resource.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = target.state;
    b.Transition.StateAfter = after;
    commands->ResourceBarrier(1, &b);
    target.state = after;
  };
  const auto area = [&](uint32_t width, uint32_t height) {
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
  };
  ID3D12DescriptorHeap* heaps[] = {present_views.Get()};
  commands->SetDescriptorHeaps(1, heaps);
  // The ramp (and the luma of FXAA) at the source size: the present root signature.
  auto& ramped = effect_targets[0];
  auto* gamma = EffectPipeline(kPresentVs, sizeof(kPresentVs), fxaa ? kPresentGammaLumaPs : kPresentGammaPs,
                               fxaa ? sizeof(kPresentGammaLumaPs) : sizeof(kPresentGammaPs), present_root.Get(),
                               ramped.format, hr);
  if (!gamma) return false;
  transition(ramped, D3D12_RESOURCE_STATE_RENDER_TARGET);
  const auto ramped_rtv = target_rtv(0);
  commands->OMSetRenderTargets(1, &ramped_rtv, FALSE, nullptr);
  area(ramped.width, ramped.height);
  commands->SetGraphicsRootSignature(present_root.Get());
  commands->SetPipelineState(gamma);
  commands->SetGraphicsRootDescriptorTable(0, view(slot));
  commands->SetGraphicsRootConstantBufferView(1, ramp);
  const uint32_t unused[8]{};
  commands->SetGraphicsRoot32BitConstants(2, 8, unused, 0);
  commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  commands->DrawInstanced(3, 1, 0, 0);
  uint32_t source = 0, source_width = stage.width, source_height = stage.height;
  // FXAA of the SDK's Swap: 16x8 threads per group, into target 1.
  if (fxaa) {
    const bool extreme = mode.post_effect == SwapPostEffect::kFxaaExtreme;
    auto* pipeline = EffectPipeline(nullptr, 0, extreme ? sdk_shaders::fxaa_extreme_cs : sdk_shaders::fxaa_cs,
                                    extreme ? sizeof(sdk_shaders::fxaa_extreme_cs) : sizeof(sdk_shaders::fxaa_cs),
                                    fxaa_root.Get(), DXGI_FORMAT_UNKNOWN, hr);
    if (!pipeline) return false;
    auto& smoothed = effect_targets[1];
    transition(ramped, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(smoothed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commands->SetComputeRootSignature(fxaa_root.Get());
    commands->SetPipelineState(pipeline);
    const uint32_t constants[4] = {stage.width, stage.height, std::bit_cast<uint32_t>(1.0f / float(stage.width)),
                                   std::bit_cast<uint32_t>(1.0f / float(stage.height))};
    commands->SetComputeRoot32BitConstants(0, 4, constants, 0);
    commands->SetComputeRootDescriptorTable(1, view(kStageSlots + 2 * 1 + 1));
    commands->SetComputeRootDescriptorTable(2, view(kStageSlots + 2 * 0));
    commands->Dispatch((stage.width + 15) / 16, (stage.height + 7) / 8, 1);
    transition(smoothed, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    source = 1;
  }
  transition(ramped, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  // The chain: the SDK's rectangle vertex shader and effect pixel shaders.
  commands->SetGraphicsRootSignature(effect_root.Get());
  commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  const float cas = -1.0f / (8.0f - 3.0f * mode.cas_sharpness);  // CasSetup const1.x.
  const float rcas = std::exp2(-mode.fsr_sharpness_reduction);  // FsrRcasCon const0.x.
  for (size_t i = 0; i < plan.size(); ++i) {
    const auto& step = plan[i];
    const bool last = i + 1 == plan.size();
    uint32_t dest_width = back_width, dest_height = back_height;
    int32_t ox = int32_t(x), oy = int32_t(y);
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = back_rtv;
    if (!last) {
      auto& target = effect_targets[2 + i];
      transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET);
      dest_width = target.width;
      dest_height = target.height;
      ox = oy = 0;
      format = target.format;
      rtv = target_rtv(uint32_t(2 + i));
    }
    commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    if (last) {
      const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
      commands->ClearRenderTargetView(rtv, black, 0, nullptr);
    }
    area(dest_width, dest_height);
    const void* ps = sdk_shaders::guest_output_bilinear_ps;
    size_t ps_size = sizeof(sdk_shaders::guest_output_bilinear_ps);
    uint32_t constants[8]{};
    const auto f = [](float value) { return std::bit_cast<uint32_t>(value); };
    switch (step.pass) {
      case OutputPass::kBilinear:
        constants[0] = uint32_t(ox), constants[1] = uint32_t(oy);
        constants[2] = f(1.0f / float(step.width)), constants[3] = f(1.0f / float(step.height));
        break;
      case OutputPass::kCasSharpen:
        ps = sdk_shaders::guest_output_ffx_cas_sharpen_ps, ps_size = sizeof(sdk_shaders::guest_output_ffx_cas_sharpen_ps);
        constants[0] = uint32_t(ox), constants[1] = uint32_t(oy), constants[2] = f(cas);
        break;
      case OutputPass::kCasResample:
        ps = sdk_shaders::guest_output_ffx_cas_resample_ps;
        ps_size = sizeof(sdk_shaders::guest_output_ffx_cas_resample_ps);
        constants[0] = uint32_t(ox), constants[1] = uint32_t(oy);
        constants[2] = f(float(source_width) / float(step.width)), constants[3] = f(float(source_height) / float(step.height));
        constants[4] = f(cas);
        break;
      case OutputPass::kFsrEasu:
        ps = sdk_shaders::guest_output_ffx_fsr_easu_ps, ps_size = sizeof(sdk_shaders::guest_output_ffx_fsr_easu_ps);
        constants[0] = f(float(source_width) / float(step.width)), constants[1] = f(float(source_height) / float(step.height));
        constants[2] = f(1.0f / float(source_width)), constants[3] = f(1.0f / float(source_height));
        break;
      case OutputPass::kFsrRcas:
        ps = sdk_shaders::guest_output_ffx_fsr_rcas_ps, ps_size = sizeof(sdk_shaders::guest_output_ffx_fsr_rcas_ps);
        constants[0] = uint32_t(ox), constants[1] = uint32_t(oy), constants[2] = f(rcas);
        break;
    }
    auto* pipeline = EffectPipeline(sdk_shaders::guest_output_triangle_strip_rect_vs,
                                    sizeof(sdk_shaders::guest_output_triangle_strip_rect_vs), ps, ps_size,
                                    effect_root.Get(), format, hr);
    if (!pipeline) return false;
    commands->SetPipelineState(pipeline);
    commands->SetGraphicsRootDescriptorTable(0, view(kStageSlots + 2 * source));
    // The rectangle in NDC, as D3D12Presenter: +Y is -V.
    const float rect[4] = {-1.0f + float(ox) * 2.0f / float(dest_width), 1.0f - float(oy) * 2.0f / float(dest_height),
                           float(step.width) * 2.0f / float(dest_width), -float(step.height) * 2.0f / float(dest_height)};
    commands->SetGraphicsRoot32BitConstants(1, 4, rect, 0);
    commands->SetGraphicsRoot32BitConstants(2, 8, constants, 0);
    commands->DrawInstanced(4, 1, 0, 0);
    if (!last) {
      transition(effect_targets[2 + i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      source = uint32_t(2 + i);
    }
    source_width = step.width;
    source_height = step.height;
  }
  return true;
}

// The UI layer (ui_pass.hlsl): t0 in a table of ui_views, the inverse
// coordinate space as root constants (b0) and the linear clamp sampler
// static; straight alpha blending into the back buffer. Each piece once, so a
// failed attempt resumes where it stopped.
bool Backend::State::EnsureUiLayer(const char*& reason, HRESULT& hr) {
  if (ui_pipeline) return true;
  auto* d3d = static_cast<plume::D3D12Device*>(device.get())->d3d;
  hr = S_OK;
  if (!ui_root) {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;  // b0: the inverse space.
    params[1].Constants.Num32BitValues = 2;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 2;
    desc.pParameters = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Microsoft::WRL::ComPtr<ID3DBlob> blob, error;
    if (FAILED(hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
        FAILED(hr = d3d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&ui_root)))) {
      reason = "native-ui-root";
      return false;
    }
  }
  if (!ui_views) {
    const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kUiViews,
                                          D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(hr = d3d->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&ui_views)))) {
      reason = "native-ui-heap";
      return false;
    }
  }
  if (!ui_textures.contains(0)) {
    const uint8_t white[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    if (!WriteUiTexture(0, 1, 1, white, reason, hr)) return false;
  }
  const D3D12_INPUT_ELEMENT_DESC elements[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(UiVertex, x), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(UiVertex, u), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(UiVertex, color), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
       0}};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
  pipeline.pRootSignature = ui_root.Get();
  pipeline.VS = {kUiVs, sizeof(kUiVs)};
  pipeline.PS = {kUiPs, sizeof(kUiPs)};
  auto& blend = pipeline.BlendState.RenderTarget[0];
  blend.BlendEnable = TRUE;
  blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
  blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
  blend.BlendOp = D3D12_BLEND_OP_ADD;
  blend.SrcBlendAlpha = D3D12_BLEND_ONE;
  blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
  blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  blend.LogicOp = D3D12_LOGIC_OP_NOOP;
  blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pipeline.SampleMask = UINT_MAX;
  pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pipeline.RasterizerState.DepthClipEnable = TRUE;
  pipeline.InputLayout = {elements, UINT(std::size(elements))};
  pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pipeline.NumRenderTargets = 1;
  pipeline.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
  pipeline.SampleDesc.Count = 1;
  if (FAILED(hr = d3d->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&ui_pipeline)))) {
    reason = "native-ui-pipeline";
    return false;
  }
  return true;
}

// A texture at a new size is a new resource: the old one is retired with its
// view while submitted work can still read it, else its view slot is reused.
// At the same size the copy goes into the texture, after the draws recorded
// before it (the barrier orders them).
bool Backend::State::WriteUiTexture(uint32_t key, uint32_t width, uint32_t height, const uint8_t* rgba,
                                    const char*& reason, HRESULT& hr) {
  auto* d3d = static_cast<plume::D3D12Device*>(device.get())->d3d;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 bytes = 0;
  d3d->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  UploadPage* page = nullptr;
  size_t page_offset = 0;
  if (!Upload(size_t(bytes), page, page_offset, hr)) {
    reason = "native-ui-texture-upload";
    return false;
  }
  for (uint32_t row = 0; row < height; ++row)
    std::memcpy(page->cpu + page_offset + footprint.Offset + size_t(row) * footprint.Footprint.RowPitch,
                rgba + size_t(row) * width * 4, size_t(width) * 4);
  auto& t = ui_textures[key];
  if (t.resource && (t.width != width || t.height != height)) {
    const uint64_t completed_now = static_cast<plume::D3D12CommandFence*>(fence.get())->d3d->GetCompletedValue();
    if (t.last_use_fence && (completed_now == UINT64_MAX || completed_now < t.last_use_fence)) {
      retired_ui_textures.push_back(std::move(t));
      t = {};
    } else {
      t.resource.Reset();
    }
  }
  const bool created = !t.resource;
  if (created) {
    if (t.view == UINT32_MAX) {
      if (!free_ui_views.empty()) {
        t.view = free_ui_views.back();
        free_ui_views.pop_back();
      } else if (next_ui_view < kUiViews) {
        t.view = next_ui_view++;
      } else {
        ui_textures.erase(key);
        reason = "native-ui-views-exhausted";
        return false;
      }
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    if (FAILED(hr = d3d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&t.resource)))) {
      free_ui_views.push_back(t.view);
      ui_textures.erase(key);
      reason = "native-ui-texture-create";
      return false;
    }
    t.width = width;
    t.height = height;
    t.state = D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    auto cpu = ui_views->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)) * t.view;
    d3d->CreateShaderResourceView(t.resource.Get(), &srv, cpu);
  }
  ID3D12GraphicsCommandList* const commands = batch_list.Get();
  auto transition = [&](D3D12_RESOURCE_STATES after) {
    if (t.state == after) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = t.resource.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = t.state;
    b.Transition.StateAfter = after;
    commands->ResourceBarrier(1, &b);
    t.state = after;
  };
  transition(D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION to{}, from{};
  to.pResource = t.resource.Get();
  to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  from.pResource = page->buffer.Get();
  from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  from.PlacedFootprint = footprint;
  from.PlacedFootprint.Offset += page_offset;
  commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  transition(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  t.last_use_fence = batch.fence;
  ++ui_texture_uploads;
  return true;
}

// The vertices and indices go into the batch's upload memory; each draw
// binds its texture and its scissor rect in pixels (UiScissorPixels).
bool Backend::State::DrawUi(ID3D12GraphicsCommandList* commands, D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t width,
                            uint32_t height, const char*& reason, HRESULT& hr) {
  const UiFrame& f = ui_frame;
  const size_t vertex_bytes = f.vertices.size() * sizeof(UiVertex);
  const size_t index_bytes = f.indices.size() * sizeof(uint16_t);
  UploadPage* page = nullptr;
  size_t offset = 0;
  if (!Upload(vertex_bytes + index_bytes, page, offset, hr)) {
    reason = "native-ui-frame-upload";
    return false;
  }
  std::memcpy(page->cpu + offset, f.vertices.data(), vertex_bytes);
  std::memcpy(page->cpu + offset + vertex_bytes, f.indices.data(), index_bytes);
  const D3D12_VERTEX_BUFFER_VIEW vertices{page->gpu + offset, UINT(vertex_bytes), UINT(sizeof(UiVertex))};
  const D3D12_INDEX_BUFFER_VIEW indices{page->gpu + offset + vertex_bytes, UINT(index_bytes), DXGI_FORMAT_R16_UINT};
  auto* d3d = static_cast<plume::D3D12Device*>(device.get())->d3d;
  const UINT increment = d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  const D3D12_VIEWPORT viewport{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
  commands->RSSetViewports(1, &viewport);
  ID3D12DescriptorHeap* heaps[] = {ui_views.Get()};
  commands->SetDescriptorHeaps(1, heaps);
  commands->SetGraphicsRootSignature(ui_root.Get());
  commands->SetPipelineState(ui_pipeline.Get());
  const float inverse_space[2] = {1.0f / f.width, 1.0f / f.height};
  commands->SetGraphicsRoot32BitConstants(1, 2, inverse_space, 0);
  commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  commands->IASetVertexBuffers(0, 1, &vertices);
  commands->IASetIndexBuffer(&indices);
  uint32_t bound = UINT32_MAX;
  for (const UiDraw& d : f.draws) {
    if (!d.index_count) continue;
    auto it = ui_textures.find(d.texture);
    if (it == ui_textures.end()) {
      ++ui_draws_skipped;
      continue;
    }
    std::array<uint32_t, 4> rect{};
    if (!UiScissorPixels(d, f, width, height, rect)) continue;
    const D3D12_RECT scissor{LONG(rect[0]), LONG(rect[1]), LONG(rect[2]), LONG(rect[3])};
    commands->RSSetScissorRects(1, &scissor);
    if (it->second.view != bound) {
      auto gpu = ui_views->GetGPUDescriptorHandleForHeapStart();
      gpu.ptr += UINT64(increment) * it->second.view;
      commands->SetGraphicsRootDescriptorTable(0, gpu);
      bound = it->second.view;
    }
    commands->DrawIndexedInstanced(d.index_count, 1, d.index_offset, INT(d.vertex_offset), 0);
    it->second.last_use_fence = batch.fence;
  }
  ++ui_frames_drawn;
  return true;
}

bool Backend::AttachWindow(const platform::NativeWindow& target, std::string& evidence, const PresentMode& mode) {
  void* window = target.kind == platform::NativeWindow::Kind::kWin32 ? target.window : nullptr;
  auto fail = [&](const char* reason, HRESULT hr = E_FAIL) {
    evidence = fmt::format("{{\"attached\":false,\"reason\":\"{}\",\"hresult\":{}}}", reason, static_cast<uint32_t>(hr));
    return false;
  };
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId() || state_->operation_poisoned ||
      state_->unproved_execution) return fail("native-attach-owner-or-state");
  if (!window || state_->swapchain) return fail("native-attach-window-or-duplicate");
  if (mode.buffers < 2 || mode.buffers > State::kMaxSwapBuffers || mode.max_frame_latency < 1 ||
      mode.max_frame_latency > 3) return fail("native-attach-present-mode");
  state_->vsync = mode.vsync;
  state_->allow_tearing = mode.allow_tearing;
  state_->swap_buffers = mode.buffers;
  state_->max_frame_latency = mode.max_frame_latency;
  state_->mode = mode;
  // The brightness exponent, 1 / (1 + (brightness - 0.5) * 1.2), on the ramp.
  state_->brightness = std::fabs(mode.brightness - 0.5f) > 0.001f;
  const double exponent = 1.0 / std::clamp(1.0 + (double(mode.brightness) - 0.5) * 1.2, 0.1, 4.0);
  for (uint32_t i = 0; i < 1024; ++i)
    state_->brightness_ramp[i] = uint16_t(std::lround(1023.0 * std::pow(i / 1023.0, exponent)));
  auto* device = static_cast<plume::D3D12Device*>(state_->device.get());
  HRESULT hr = S_OK;
  if (!state_->present_root) {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;  // b0: the ramp.
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;  // b1: origin, scale, size, extent.
    params[2].Constants.ShaderRegister = 1;
    params[2].Constants.Num32BitValues = 8;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 3;
    desc.pParameters = params;
    Microsoft::WRL::ComPtr<ID3DBlob> blob, error;
    if (FAILED(hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
        FAILED(hr = device->d3d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                     IID_PPV_ARGS(&state_->present_root))))
      return fail("native-present-root", hr);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = state_->present_root.Get();
    pipeline.VS = {kPresentVs, sizeof(kPresentVs)};
    pipeline.PS = {kPresentPs, sizeof(kPresentPs)};
    pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pipeline.SampleMask = UINT_MAX;
    pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pipeline.RasterizerState.DepthClipEnable = TRUE;
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets = 1;
    pipeline.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    pipeline.SampleDesc.Count = 1;
    if (FAILED(hr = device->d3d->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&state_->present_pipeline))))
      return fail("native-present-pipeline", hr);
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, State::kPresentViews,
                                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(hr = device->d3d->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&state_->present_views))))
      return fail("native-present-heap", hr);
    const D3D12_DESCRIPTOR_HEAP_DESC targets{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, State::kEffectTargets,
                                             D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(hr = device->d3d->CreateDescriptorHeap(&targets, IID_PPV_ARGS(&state_->effect_rtv))))
      return fail("native-present-effect-heap", hr);
  }
  auto swapchain = state_->queue->createSwapChain(plume::RenderSwapChainDesc(
      static_cast<HWND>(window), plume::RenderFormat::B8G8R8A8_UNORM, state_->swap_buffers, false,
      state_->max_frame_latency));
  auto* native = static_cast<plume::D3D12SwapChain*>(swapchain.get());
  if (!swapchain || !native->d3d) return fail("native-swapchain-create");
  state_->swapchain = std::move(swapchain);
  state_->window = window;
  state_->swapchain->setVsyncEnabled(state_->vsync);
  if (!state_->BackBufferViews(hr)) {
    state_->present_rtv.Reset();
    state_->swapchain.reset();
    state_->window = nullptr;
    return fail("native-swapchain-views", hr);
  }
  DXGI_SWAP_CHAIN_DESC1 desc{};
  native->d3d->GetDesc1(&desc);
  evidence = fmt::format("{{\"attached\":true,\"window\":{},\"size\":[{},{}],\"format\":{},\"buffers\":{},"
                         "\"swap_effect\":{},\"flags\":{},\"tearing_allowed\":{},\"waitable\":{},\"vsync\":{},"
                         "\"tearing_option\":{},\"max_frame_latency\":{},\"brightness\":{},\"effect\":{},"
                         "\"post_effect\":{}}}",
                         reinterpret_cast<uintptr_t>(window), desc.Width, desc.Height, static_cast<uint32_t>(desc.Format),
                         desc.BufferCount, static_cast<uint32_t>(desc.SwapEffect), desc.Flags,
                         (desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0, native->waitableObject != nullptr,
                         state_->vsync, state_->allow_tearing, state_->max_frame_latency, mode.brightness,
                         uint32_t(mode.effect), uint32_t(mode.post_effect));
  return true;
}

bool Backend::DetachWindow(std::string& evidence) {
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId()) {
    evidence = "{\"detached\":false,\"reason\":\"native-detach-owner\"}";
    return false;
  }
  if (!state_->swapchain) {
    evidence = "{\"detached\":false,\"reason\":\"native-detach-no-swapchain\"}";
    return false;
  }
  // The back buffers and stage slots are released only after every operation
  // that could still use them completed.
  std::string idle;
  if (!WaitIdle(idle)) {
    evidence = fmt::format("{{\"detached\":false,\"reason\":\"native-detach-wait\",\"idle\":{}}}", idle);
    return false;
  }
  state_->present_rtv.Reset();
  state_->swapchain.reset();
  state_->window = nullptr;
  uint32_t stages = 0;
  for (auto& stage : state_->stages) {
    stages += stage.resource != nullptr;
    stage = {};
  }
  state_->ReleaseEffects();  // Their last use completed above.
  evidence = fmt::format("{{\"detached\":true,\"presents\":{},\"skipped_empty\":{},\"resizes\":{},\"captures\":{},"
                         "\"latency_waits\":{},\"latency_timeouts\":{},\"stage_slots_released\":{},\"idle\":{}}}",
                         state_->presents, state_->presents_skipped_empty, state_->resizes, state_->captures,
                         state_->latency_waits, state_->latency_timeouts, stages, idle);
  return true;
}

bool Backend::SetGammaRamp(const std::array<uint16_t, 768>& words, std::string& evidence) {
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId()) {
    evidence = "{\"gamma\":false,\"reason\":\"native-gamma-owner\"}";
    return false;
  }
  // D3DGAMMARAMP: 10-bit values in the high bits; the low 6 bits must be zero.
  for (uint16_t word : words)
    if (word & 0x3F) {
      evidence = fmt::format("{{\"gamma\":false,\"reason\":\"native-gamma-low-bits\",\"word\":{}}}", word);
      return false;
    }
  for (size_t i = 0; i < words.size(); ++i) state_->gamma[i] = words[i] >> 6;
  state_->gamma_from_title = true;
  ++state_->gamma_updates;
  evidence = fmt::format("{{\"gamma\":true,\"updates\":{},\"r\":[{},{},{}],\"g\":[{},{},{}],\"b\":[{},{},{}]}}",
                         state_->gamma_updates, state_->gamma[0], state_->gamma[128], state_->gamma[255],
                         state_->gamma[256], state_->gamma[384], state_->gamma[511], state_->gamma[512],
                         state_->gamma[640], state_->gamma[767]);
  return true;
}

bool Backend::SetVsync(bool enabled, std::string& evidence) {
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId()) {
    evidence = "{\"vsync\":false,\"reason\":\"native-vsync-owner\"}";
    return false;
  }
  state_->vsync = enabled;
  if (state_->swapchain) state_->swapchain->setVsyncEnabled(enabled);
  evidence = fmt::format("{{\"vsync\":{},\"swapchain\":{}}}", enabled, state_->swapchain != nullptr);
  return true;
}

bool Backend::StageFront(uint32_t key, uint32_t& slot, std::string& evidence) {
  slot = UINT32_MAX;
  auto fail = [&](const char* reason, HRESULT hr = E_FAIL) {
    evidence = fmt::format("{{\"staged\":false,\"reason\":\"{}\",\"key\":{},\"hresult\":{}}}", reason, key,
                           static_cast<uint32_t>(hr));
    return false;
  };
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId() || state_->operation_poisoned ||
      state_->unproved_execution) return fail("native-stage-owner-or-state");
  if (!state_->present_views) return fail("native-stage-without-window");
  // Source: an own texture with defined content, else a single-sample color surface.
  ID3D12Resource* source = nullptr;
  D3D12_RESOURCE_STATES* source_state = nullptr;
  uint64_t* source_last_use = nullptr;
  if (auto it = state_->textures.find(key); it != state_->textures.end() && it->second.texture) {
    if (it->second.depth || !it->second.content_defined) return fail("native-present-content-undefined");
    source = static_cast<plume::D3D12Texture*>(it->second.texture.get())->d3d;
    source_state = &it->second.state;
    source_last_use = &it->second.last_use_fence;
  } else if (auto s = state_->surfaces.find(key); s != state_->surfaces.end() && s->second.texture &&
             s->second.desc.format == SurfaceFormat::Color8888 && s->second.desc.samples == 1) {
    source = static_cast<plume::D3D12Texture*>(s->second.texture.get())->d3d;
    source_state = &s->second.state;
    source_last_use = &s->second.last_use_fence;
  } else {
    return fail("native-stage-source-unknown");
  }
  const auto source_desc = source->GetDesc();
  if (source_desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM || source_desc.SampleDesc.Count != 1 ||
      source_desc.MipLevels != 1)
    return fail("native-stage-source-format");
  const uint32_t index = uint32_t(state_->staged_frames % State::kStageSlots);
  auto& stage = state_->stages[index];
  // Two slots suffice: frame N + 2 is staged only after the Swap of N + 1
  // waited for the fence of N, which the flip of N publishes after posting its
  // Present. A staged slot not yet presented is refused, never overwritten.
  if (stage.staged && !stage.presented) return fail("native-stage-slot-not-presented");
  auto* device = static_cast<plume::D3D12Device*>(state_->device.get());
  auto* fence = static_cast<plume::D3D12CommandFence*>(state_->fence.get());
  HRESULT hr = S_OK;
  if (!stage.resource || stage.width != source_desc.Width || stage.height != source_desc.Height) {
    if (stage.resource && stage.last_use_fence && fence->d3d->GetCompletedValue() < stage.last_use_fence)
      return fail("native-stage-resize-in-flight");
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc = source_desc;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    stage = {};
    if (FAILED(hr = device->d3d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&stage.resource))))
      return fail("native-stage-create", hr);
    stage.width = uint32_t(desc.Width);
    stage.height = desc.Height;
    stage.state = D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    auto cpu = state_->present_views->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(device->d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)) * index;
    device->d3d->CreateShaderResourceView(stage.resource.Get(), &srv, cpu);
  }
  const char* batch_reason = nullptr;
  if (!state_->OpenBatch(batch_reason, hr)) return fail(batch_reason, hr);
  ID3D12GraphicsCommandList* const commands = state_->batch_list.Get();
  auto barrier = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES after) {
    if (state == after) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = state;
    b.Transition.StateAfter = after;
    commands->ResourceBarrier(1, &b);
    state = after;
  };
  barrier(source, *source_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  barrier(stage.resource.Get(), stage.state, D3D12_RESOURCE_STATE_COPY_DEST);
  const auto copies = PresentCopies(state_->render_mode.scale, stage.width, stage.height);
  if (copies.size() == 1) {
    commands->CopyResource(stage.resource.Get(), source);
  } else {
    D3D12_TEXTURE_COPY_LOCATION to{}, from{};
    to.pResource = stage.resource.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.pResource = source; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    for (const GapCopy& g : copies) {
      const D3D12_BOX box{g.source.left, g.source.top, 0, g.source.right, g.source.bottom, 1};
      commands->CopyTextureRegion(&to, g.dest_x, g.dest_y, 0, &from, &box);
    }
  }
  *source_last_use = stage.last_use_fence = state_->batch.fence;
  stage.key = key;
  stage.staged = true;
  stage.presented = false;
  slot = index;
  ++state_->staged_frames;
  ++state_->batch.operations;
  ++state_->operations;
  evidence = fmt::format("{{\"staged\":true,\"key\":{},\"slot\":{},\"size\":[{},{}],\"fence\":{},\"staged_frames\":{}}}",
                         key, slot, stage.width, stage.height, stage.last_use_fence, state_->staged_frames);
  return true;
}

bool Backend::Present(uint32_t slot, bool capture, std::vector<uint8_t>& bgra, uint32_t& width,
                                   uint32_t& height, std::string& evidence, bool& presented) {
  presented = false;
  bgra.clear();
  width = height = 0;
  auto fail = [&](const char* reason, HRESULT hr = E_FAIL) {
    evidence = fmt::format("{{\"presented\":false,\"reason\":\"{}\",\"slot\":{},\"hresult\":{}}}", reason, slot,
                           static_cast<uint32_t>(hr));
    return false;
  };
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId() || state_->operation_poisoned ||
      state_->unproved_execution) return fail("native-present-owner-or-state");
  if (!state_->swapchain) return fail("native-present-no-swapchain");
  if (slot >= State::kStageSlots || !state_->stages[slot].staged) return fail("native-present-slot-not-staged");
  auto& stage = state_->stages[slot];
  auto* native = static_cast<plume::D3D12SwapChain*>(state_->swapchain.get());
  auto* device = static_cast<plume::D3D12Device*>(state_->device.get());
  HRESULT hr = S_OK;
  // The swapchain follows the window: a new client size resizes the buffers
  // once no submitted work can still use them.
  bool resized = false;
  if (state_->swapchain->needsResize()) {
    std::string idle;
    if (!WaitIdle(idle)) return fail("native-present-resize-wait");
    state_->present_rtv.Reset();
    if (state_->swapchain->resize()) {
      if (!state_->BackBufferViews(hr)) return fail("native-present-resize-views", hr);
      ++state_->resizes;
      resized = true;
    } else if (!state_->swapchain->isEmpty()) {
      return fail("native-present-resize");
    }
  }
  if (state_->swapchain->isEmpty() || !state_->present_rtv) {
    stage.presented = true;  // A minimized window shows nothing; the frame is not kept.
    ++state_->presents_skipped_empty;
    evidence = fmt::format("{{\"presented\":false,\"skipped\":\"window-empty\",\"slot\":{},\"skipped_empty\":{}}}",
                           slot, state_->presents_skipped_empty);
    return true;
  }
  // Frame latency (1 by default): wait (bounded) until the queue has room for this Present.
  LARGE_INTEGER t0{}, t1{}, t2{}, frequency{};
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&t0);
  if (native->waitableObject) {
    ++state_->latency_waits;
    if (WaitForSingleObject(native->waitableObject, 100) != WAIT_OBJECT_0) ++state_->latency_timeouts;
  }
  QueryPerformanceCounter(&t1);
  const uint32_t w = native->getWidth(), h = native->getHeight();
  // Largest rect of the image's aspect, centered; the rest is black.
  const double fit = std::min(double(w) / stage.width, double(h) / stage.height);
  const uint32_t vw = std::clamp<uint32_t>(uint32_t(std::lround(stage.width * fit)), 1, w);
  const uint32_t vh = std::clamp<uint32_t>(uint32_t(std::lround(stage.height * fit)), 1, h);
  const uint32_t vx = (w - vw) / 2, vy = (h - vh) / 2;
  // The output effects and their targets, (re)created once no submitted
  // work can still read the old ones or their views.
  const bool effects = state_->mode.effect != PresentEffect::kBilinear ||
                       state_->mode.post_effect != SwapPostEffect::kNone;
  std::vector<OutputStep> plan;
  std::string passes = "present";
  if (effects) {
    plan = OutputPlan(state_->mode, stage.width, stage.height, vw, vh);
    if (plan.size() > State::kEffectTargets - 1) return fail("native-present-effects-plan");  // Five intermediates.
    const bool fxaa = state_->mode.post_effect != SwapPostEffect::kNone;
    struct Need {
      uint32_t i, width, height;
      DXGI_FORMAT format;
      bool uav;
    };
    std::vector<Need> needs{{0, stage.width, stage.height,
                             fxaa ? DXGI_FORMAT_R16G16B16A16_UNORM : DXGI_FORMAT_R10G10B10A2_UNORM, false}};
    if (fxaa) needs.push_back({1, stage.width, stage.height, DXGI_FORMAT_R10G10B10A2_UNORM, true});
    for (size_t i = 0; i + 1 < plan.size(); ++i)
      needs.push_back({uint32_t(2 + i), plan[i].width, plan[i].height, DXGI_FORMAT_R10G10B10A2_UNORM, false});
    bool stale = false;
    for (const auto& n : needs) {
      const auto& target = state_->effect_targets[n.i];
      stale |= target.resource && (target.width != n.width || target.height != n.height || target.format != n.format);
    }
    if (stale) {
      std::string idle;
      if (!WaitIdle(idle)) return fail("native-present-effects-wait");
    }
    for (const auto& n : needs)
      if (!state_->EnsureEffectTarget(n.i, n.width, n.height, n.format, n.uav, hr))
        return fail("native-present-effects-target", hr);
    if (!state_->EffectRoots(hr)) return fail("native-present-effects-root", hr);
    passes = fxaa ? "ramp+fxaa" : "ramp";
    for (const auto& step : plan) passes += fmt::format("+{}:{}x{}", OutputPassName(step.pass), step.width, step.height);
  }
  uint32_t index = 0;
  if (!state_->swapchain->acquireTexture(nullptr, &index) || index >= state_->swap_buffers)
    return fail("native-present-acquire");
  auto* back = static_cast<plume::D3D12Texture*>(state_->swapchain->getTexture(index));
  const char* batch_reason = nullptr;
  if (!state_->OpenBatch(batch_reason, hr)) return fail(batch_reason, hr);
  const bool ui = state_->UiFrameVisible();
  if (ui && !state_->EnsureUiLayer(batch_reason, hr)) return fail(batch_reason, hr);
  ID3D12GraphicsCommandList* const commands = state_->batch_list.Get();
  auto barrier = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    commands->ResourceBarrier(1, &b);
  };
  // The ramp: 256 x uint4 (r, g, b, 0) in the batch's upload memory.
  State::UploadPage* page = nullptr;
  size_t offset = 0;
  if (!state_->Upload(256 * 16, page, offset, hr)) return fail("native-present-ramp-upload", hr);
  auto* ramp = reinterpret_cast<uint32_t*>(page->cpu + offset);
  // The brightness exponent on the 10-bit values, unless it is 0.5.
  const auto level = [&](uint32_t value) {
    return state_->brightness ? uint32_t(state_->brightness_ramp[std::min(value, 1023u)]) : value;
  };
  for (uint32_t i = 0; i < 256; ++i) {
    ramp[4 * i + 0] = level(state_->gamma[i]);
    ramp[4 * i + 1] = level(state_->gamma[256 + i]);
    ramp[4 * i + 2] = level(state_->gamma[512 + i]);
    ramp[4 * i + 3] = 0;
  }
  barrier(stage.resource.Get(), stage.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  stage.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  barrier(back->d3d, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
  auto rtv = state_->present_rtv->GetCPUDescriptorHandleForHeapStart();
  rtv.ptr += SIZE_T(device->d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)) * index;
  // Invalidate before recording, including an effects/UI path that may fail.
  state_->batch.draw_bindings = false;
  commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  if (effects) {
    if (!state_->OutputEffects(commands, slot, rtv, w, h, vx, vy, plan, page->gpu + offset, hr))
      return fail("native-present-effects", hr);
  } else {
    // The pass covers the whole back buffer and writes the black bars itself.
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, float(w), float(h), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, LONG(w), LONG(h)};
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    ID3D12DescriptorHeap* heaps[] = {state_->present_views.Get()};
    commands->SetDescriptorHeaps(1, heaps);
    commands->SetGraphicsRootSignature(state_->present_root.Get());
    commands->SetPipelineState(state_->present_pipeline.Get());
    auto gpu = state_->present_views->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += UINT64(device->d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)) * slot;
    commands->SetGraphicsRootDescriptorTable(0, gpu);
    commands->SetGraphicsRootConstantBufferView(1, page->gpu + offset);
    const float pass[4] = {float(vx), float(vy), float(stage.width) / float(vw), float(stage.height) / float(vh)};
    const uint32_t size[2] = {stage.width, stage.height};
    const float extent[2] = {float(vw), float(vh)};
    commands->SetGraphicsRoot32BitConstants(2, 4, pass, 0);
    commands->SetGraphicsRoot32BitConstants(2, 2, size, 4);
    commands->SetGraphicsRoot32BitConstants(2, 2, extent, 6);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->DrawInstanced(3, 1, 0, 0);
  }
  // The UI layer over the image and its bars.
  if (ui) {
    if (!state_->DrawUi(commands, rtv, w, h, batch_reason, hr)) return fail(batch_reason, hr);
    passes += "+ui";
  }
  Microsoft::WRL::ComPtr<ID3D12Resource> readback;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 total = 0;
  if (capture) {
    const auto desc = back->d3d->GetDesc();
    UINT rows = 0;
    UINT64 row_bytes = 0;
    device->d3d->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &row_bytes, &total);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(hr = device->d3d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&readback))))
      return fail("native-present-capture-buffer", hr);
    barrier(back->d3d, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION to{}, from{};
    to.pResource = readback.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = footprint;
    from.pResource = back->d3d;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    barrier(back->d3d, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    state_->batch.retained.push_back(readback);
  } else {
    barrier(back->d3d, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
  }
  stage.last_use_fence = state_->batch.fence;
  const uint64_t present_fence = state_->batch.fence;
  ++state_->batch.operations;
  ++state_->operations;
  if (!state_->CloseBatch(batch_reason, hr)) return fail(batch_reason, hr);
  // The interval and flags of the PresentMode, in place of Plume's present,
  // which tears whenever the system allows it. Tearing also needs the swapchain flag.
  DXGI_SWAP_CHAIN_DESC1 chain{};
  native->d3d->GetDesc1(&chain);
  const bool tearing = !state_->vsync && state_->allow_tearing &&
      (chain.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
  if (FAILED(hr = native->d3d->Present(state_->vsync ? 1 : 0, tearing ? DXGI_PRESENT_ALLOW_TEARING : 0)))
    return fail("native-present-swapchain", hr);
  const bool visible = hr == S_OK;
  QueryPerformanceCounter(&t2);
  // The Present is queue work that still uses the back buffer after the
  // batch's Signal. A Signal after it lets WaitIdle (resize, capture, detach,
  // Shutdown) prove it complete (else D3D12 #921 OBJECT_DELETED_WHILE_STILL_IN_USE
  // on the swapchain release).
  {
    auto* queue = static_cast<plume::D3D12CommandQueue*>(state_->queue.get());
    auto* fence = static_cast<plume::D3D12CommandFence*>(state_->fence.get());
    const uint64_t after = fence->fenceValue;
    if (!after || after == UINT64_MAX) return fail("native-present-fence-exhausted");
    if (FAILED(hr = queue->d3d->Signal(fence->d3d, after))) {
      state_->operation_poisoned = true;
      return fail("native-present-signal", hr);
    }
    state_->pending_signal = after;
    ++fence->fenceValue;
  }
  stage.presented = true;
  ++state_->presents;
  if (capture) {
    std::string idle;
    if (!WaitIdle(idle)) return fail("native-present-capture-wait");
    uint8_t* mapped = nullptr;
    const D3D12_RANGE range{0, SIZE_T(total)};  // The last row is not padded to the pitch.
    if (FAILED(hr = readback->Map(0, &range, reinterpret_cast<void**>(&mapped))))
      return fail("native-present-capture-map", hr);
    width = footprint.Footprint.Width;
    height = footprint.Footprint.Height;
    bgra.resize(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y)
      std::memcpy(&bgra[size_t(y) * width * 4], mapped + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch,
                  size_t(width) * 4);
    const D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    ++state_->captures;
  }
  const auto us = [&](const LARGE_INTEGER& a, const LARGE_INTEGER& b) {
    return (b.QuadPart - a.QuadPart) * 1000000 / frequency.QuadPart;
  };
  presented = visible;
  evidence = fmt::format("{{\"presented\":{},\"slot\":{},\"key\":{},\"buffer\":{},\"size\":[{},{}],"
                         "\"image\":[{},{},{},{}],\"vsync\":{},\"tearing\":{},\"resized\":{},\"fence\":{},"
                         "\"latency_wait_us\":{},\"present_us\":{},\"gamma_from_title\":{},\"gamma_updates\":{},"
                         "\"presents\":{},\"captured\":{},\"passes\":\"{}\"}}",
                         presented, slot, stage.key, index, w, h, vx, vy, vw, vh, state_->vsync, tearing, resized, present_fence,
                         us(t0, t1), us(t1, t2), state_->gamma_from_title, state_->gamma_updates, state_->presents,
                         capture, passes);
  return true;
}

bool Backend::UploadUiTexture(uint32_t key, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba,
                              std::string& evidence) {
  auto fail = [&](const char* reason, HRESULT hr = E_FAIL) {
    evidence = fmt::format("{{\"uploaded\":false,\"reason\":\"{}\",\"key\":{},\"hresult\":{}}}", reason, key,
                           static_cast<uint32_t>(hr));
    return false;
  };
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId() || state_->operation_poisoned ||
      state_->unproved_execution) return fail("native-ui-owner-or-state");
  HRESULT hr = S_OK;
  const char* reason = nullptr;
  if (!state_->OpenBatch(reason, hr) || !state_->EnsureUiLayer(reason, hr) ||
      !state_->WriteUiTexture(key, width, height, rgba.data(), reason, hr))
    return fail(reason, hr);
  const uint64_t batch_operations = ++state_->batch.operations;
  ++state_->operations;
  if (batch_operations >= kBatchOperationsMax && !state_->CloseBatch(reason, hr)) return fail(reason, hr);
  evidence = fmt::format("{{\"uploaded\":true,\"key\":{},\"size\":[{},{}],\"fence\":{}}}", key, width, height,
                         state_->ui_textures[key].last_use_fence);
  return true;
}

bool Backend::ReleaseUiTexture(uint32_t key, std::string& evidence) {
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId()) {
    evidence = "{\"released\":false,\"reason\":\"native-ui-release-owner\"}";
    return false;
  }
  // Key 0 (the white texture) belongs to the layer.
  auto it = key ? state_->ui_textures.find(key) : state_->ui_textures.end();
  if (it == state_->ui_textures.end()) {
    evidence = fmt::format("{{\"released\":true,\"key\":{},\"ui_texture\":false}}", key);
    return true;
  }
  const uint64_t completed = static_cast<plume::D3D12CommandFence*>(state_->fence.get())->d3d->GetCompletedValue();
  const uint64_t last_use = it->second.last_use_fence;
  const bool in_flight = last_use && (completed == UINT64_MAX || completed < last_use);
  if (in_flight)
    state_->retired_ui_textures.push_back(std::move(it->second));
  else
    state_->free_ui_views.push_back(it->second.view);
  state_->ui_textures.erase(it);
  evidence = fmt::format("{{\"released\":true,\"key\":{},\"ui_texture\":true,\"deferred\":{},\"last_use_fence\":{}}}",
                         key, in_flight, last_use);
  return true;
}

bool Backend::SetUiFrame(UiFrame frame, std::string& evidence) {
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId()) {
    evidence = "{\"set\":false,\"reason\":\"native-ui-frame-owner\"}";
    return false;
  }
  state_->ui_frame = std::move(frame);
  evidence = fmt::format("{{\"set\":true,\"vertices\":{},\"indices\":{},\"draws\":{}}}",
                         state_->ui_frame.vertices.size(), state_->ui_frame.indices.size(),
                         state_->ui_frame.draws.size());
  return true;
}

std::string Backend::ResourceCounts() {
  uint32_t textures = 0, stages = 0;
  for (const auto& [key, texture] : state_->textures) textures += texture.texture != nullptr;
  for (const auto& stage : state_->stages) stages += stage.resource != nullptr;
  return fmt::format("{{\"textures\":{},\"texture_entries\":{},\"sampled\":{},\"surfaces\":{},\"retired_surfaces\":{},"
                     "\"retired_textures\":{},\"textures_released\":{},\"textures_retired\":{},\"retired_sampled\":{},"
                     "\"sampled_released\":{},\"sampled_retired\":{},\"views_allocated\":{},\"views_free\":{},"
                     "\"stage_slots\":{},\"swapchain\":{},\"depth_resolves\":{},\"sample_resolves\":{},\"buffers\":{},"
                     "\"retired_buffers\":{},"
                     "\"buffers_created\":{},\"buffers_released\":{},\"buffers_retired\":{},\"buffer_uploads\":{},"
                     "\"buffer_heaps\":{},\"buffer_blocks_used\":{},\"ui_textures\":{},\"retired_ui_textures\":{},"
                     "\"ui_views_free\":{},\"ui_texture_uploads\":{},\"ui_frames_drawn\":{},\"ui_draws_skipped\":{}}}",
                     textures, state_->textures.size(), state_->sampled.size(),
                     state_->surfaces.size(), state_->retired_surfaces.size(), state_->retired_textures.size(),
                     state_->textures_released, state_->textures_retired, state_->retired_sampled.size(),
                     state_->sampled_released, state_->sampled_retired, state_->next_view - 1, state_->free_views.size(),
                     stages, state_->swapchain != nullptr, state_->depth_resolves, state_->sample_resolves,
                     state_->buffers.size(),
                     state_->retired_buffers.size(), state_->buffers_created, state_->buffers_released,
                     state_->buffers_retired, state_->buffer_uploads, state_->buffer_heaps.size(),
                     state_->buffer_blocks_used, state_->ui_textures.size() - state_->ui_textures.count(0),
                     state_->retired_ui_textures.size(), state_->free_ui_views.size(), state_->ui_texture_uploads,
                     state_->ui_frames_drawn, state_->ui_draws_skipped);
}

bool Backend::LiveResources(ResourceKeys& keys, std::string& evidence) {
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId()) {
    evidence = "{\"released\":false,\"reason\":\"native-release-remaining-owner\"}";
    return false;
  }
  for (const auto& [key, surface] : state_->surfaces) keys.surfaces.push_back(key);
  for (const auto& [key, texture] : state_->textures) keys.textures.push_back(key);
  for (const auto& [address, copy] : state_->sampled) keys.sampled.push_back(address);
  for (const auto& [key, buffer] : state_->buffers) keys.buffers.push_back(key);
  std::sort(keys.buffers.begin(), keys.buffers.end());
  std::sort(keys.surfaces.begin(), keys.surfaces.end());
  std::sort(keys.textures.begin(), keys.textures.end());
  std::sort(keys.sampled.begin(), keys.sampled.end());
  return true;
}

bool Backend::ReleaseTexture(uint32_t key, std::string& evidence) {
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId()) {
    evidence = "{\"released\":false,\"reason\":\"native-texture-release-owner\"}";
    return false;
  }
  auto it = state_->textures.find(key);
  if (it == state_->textures.end()) {
    evidence = fmt::format("{{\"released\":true,\"key\":{},\"own_texture\":false}}", key);
    return true;
  }
  auto* fence = static_cast<plume::D3D12CommandFence*>(state_->fence.get());
  const uint64_t completed = fence->d3d->GetCompletedValue();
  // The open batch has not been submitted: its fence is above every completed value.
  const uint64_t last_use = it->second.last_use_fence;
  const bool in_flight = last_use && (completed == UINT64_MAX || completed < last_use);
  const uint32_t view = it->second.view;
  const bool had_texture = it->second.texture != nullptr;
  if (in_flight) {
    state_->retired_textures.push_back(std::move(it->second));
    ++state_->textures_retired;
  } else {
    if (view) state_->free_views.push_back(view);
    if (had_texture) ++state_->textures_released;
  }
  state_->textures.erase(it);
  evidence = fmt::format("{{\"released\":true,\"key\":{},\"own_texture\":{},\"deferred\":{},\"last_use_fence\":{},"
                         "\"observed_completed\":{},\"view\":{},\"counts\":{}}}", key, had_texture, in_flight, last_use,
                         completed, view, ResourceCounts());
  return true;
}

bool Backend::ReleaseSampled(uint32_t address, std::string& evidence) {
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId()) {
    evidence = "{\"released\":false,\"reason\":\"native-sampled-release-owner\"}";
    return false;
  }
  auto it = state_->sampled.find(address);
  if (it == state_->sampled.end()) {
    evidence = fmt::format("{{\"released\":true,\"address\":{},\"sampled\":false}}", address);
    return true;
  }
  auto* fence = static_cast<plume::D3D12CommandFence*>(state_->fence.get());
  const uint64_t completed = fence->d3d->GetCompletedValue();
  // The open batch has not been submitted: its fence is above every completed value.
  const uint64_t last_use = it->second.last_use_fence;
  const bool in_flight = last_use && (completed == UINT64_MAX || completed < last_use);
  const uint32_t view = it->second.view;
  if (in_flight) {
    state_->retired_sampled.push_back(std::move(it->second));
    ++state_->sampled_retired;
  } else {
    if (view) state_->free_views.push_back(view);
    ++state_->sampled_released;
  }
  state_->sampled.erase(it);
  evidence = fmt::format("{{\"released\":true,\"address\":{},\"sampled\":true,\"deferred\":{},\"last_use_fence\":{},"
                         "\"observed_completed\":{},\"view\":{},\"counts\":{}}}", address, in_flight, last_use, completed,
                         view, ResourceCounts());
  return true;
}


bool Backend::ResolveSurface(const ResolveRequest& request, uint64_t& fenceValue,
                                          std::string& evidence) {
  profile::Timer profile(profile::kEngineResolve);
  fenceValue = 0;
  auto fail = [&](const char* reason, HRESULT hr = E_FAIL, const std::string& fields = {}) {
    evidence = fmt::format("{{\"resolved\":false,\"reason\":\"{}\",\"hresult\":{}{}{}}}", reason,
                           static_cast<uint32_t>(hr), fields.empty() ? "" : ",", fields);
    return false;
  };
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId() || state_->operation_poisoned ||
      state_->unproved_execution) return fail("native-resolve-owner-or-state");
  auto source_it = state_->surfaces.find(request.source_key);
  if (source_it == state_->surfaces.end()) return fail("native-resolve-source-unknown");
  auto& source = source_it->second;
  const bool depth = source.desc.format == SurfaceFormat::DepthStencil;
  if (!request.dest_key || !request.dest_width || !request.dest_height) return fail("native-resolve-destination");
  // The request is in guest pixels; the destination is as scaled as the source.
  const HostResolve host = ResolveInHost(state_->render_mode.scale, request);
  const uint32_t dest_width = host.dest_width, dest_height = host.dest_height;
  // One sample of an MSAA color source goes through the pass, as the depth
  // does; ResolveSubresource would average it with the others.
  const bool sampled = !depth && source.desc.samples > 1 && request.sample >= 0;
  // Own destination texture keyed by the guest texture header; recreated when
  // the header describes another size, or when a pass needs it as a render target.
  auto& dest = state_->textures[request.dest_key];
  auto* device = static_cast<plume::D3D12Device*>(state_->device.get());
  if (!dest.texture || dest.width != dest_width || dest.height != dest_height || dest.depth != depth ||
      (sampled && !dest.render_target)) {
    // Submitted or recorded work may still use the old texture (the open batch
    // has not been submitted, so its fence is above every completed value):
    // it waits for that fence with its view (ReleaseRetiredTextures), and the
    // draw gives the new one a view of its own. Otherwise the draw rewrites
    // the SRV of the slot for the new resource.
    const uint64_t completed = static_cast<plume::D3D12CommandFence*>(state_->fence.get())->d3d->GetCompletedValue();
    if (dest.texture && dest.last_use_fence && (completed == UINT64_MAX || completed < dest.last_use_fence)) {
      state_->retired_textures.push_back(std::move(dest));
      ++state_->textures_retired;
      dest = {};
    } else {
      const uint32_t view = dest.view;
      dest = {};
      dest.view = view;
    }
    // A depth destination, or the one of a color sample, is the render target of its pass.
    auto texture_desc = plume::RenderTextureDesc::Texture2D(
        dest_width, dest_height, 1, depth ? plume::RenderFormat::R32_FLOAT : plume::RenderFormat::R8G8B8A8_UNORM,
        depth || sampled ? plume::RenderTextureFlag::RENDER_TARGET : plume::RenderTextureFlag::NONE);
    // Committed, as Plume makes its targets: sub-allocated copy destinations
    // and render targets were seen corrupted on some drivers (AMD before
    // 32.0.13031.8021, Intel Gen9.5 and Gen11; Dawn's
    // disable_sub_allocation_for_2d_texture_with_copy_dst_or_render_attachment).
    texture_desc.committed = true;
    dest.texture = state_->device->createTexture(texture_desc);
    if (!dest.texture) return fail("native-resolve-destination-create");
    dest.width = dest_width; dest.height = dest_height; dest.depth = depth; dest.render_target = depth || sampled;
    dest.state = static_cast<plume::D3D12Texture*>(dest.texture.get())->resourceStates;
  }
  const bool same_size = source.desc.width == dest.width && source.desc.height == dest.height;
  // The deferred cases (content left undefined with success) are refused:
  // the draw layer stops in a guard before reaching them.
  if (const char* uncovered = ResolveUncovered(request, depth, source.desc.samples, same_size)) return fail(uncovered);
  // Recorded into the open batch, complete with its fence.
  HRESULT hr = S_OK;
  const char* batch_reason = nullptr;
  if (!state_->OpenBatch(batch_reason, hr)) return fail(batch_reason, hr);
  ID3D12GraphicsCommandList* const commands = state_->batch_list.Get();
  auto barrier = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES after) {
    if (state == after) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = state;
    b.Transition.StateAfter = after;
    commands->ResourceBarrier(1, &b);
    state = after;
  };
  auto* src = static_cast<plume::D3D12Texture*>(source.texture.get())->d3d;
  auto* dst = static_cast<plume::D3D12Texture*>(dest.texture.get())->d3d;
  const char* method = "copy-region";
  if (depth || sampled) {
    // Full-screen pass: the depth of the selected sample (or the minimum of
    // all) goes to the R32_FLOAT destination as a render target; one color
    // sample to the R8G8B8A8_UNORM destination.
    if (!state_->depth_resolve_root) {
      D3D12_DESCRIPTOR_RANGE range{};
      range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      range.NumDescriptors = 1;
      D3D12_ROOT_PARAMETER params[2]{};
      params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      params[0].DescriptorTable.NumDescriptorRanges = 1;
      params[0].DescriptorTable.pDescriptorRanges = &range;
      params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;  // b0: sample index, samples.
      params[1].Constants.Num32BitValues = 2;
      params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      D3D12_ROOT_SIGNATURE_DESC desc{};
      desc.NumParameters = 2;
      desc.pParameters = params;
      Microsoft::WRL::ComPtr<ID3DBlob> blob, error;
      if (FAILED(hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
          FAILED(hr = device->d3d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                       IID_PPV_ARGS(&state_->depth_resolve_root))))
        return fail("native-depth-resolve-root", hr);
      D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
      pipeline.pRootSignature = state_->depth_resolve_root.Get();
      pipeline.VS = {kDepthResolveVs, sizeof(kDepthResolveVs)};
      pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      pipeline.SampleMask = UINT_MAX;
      pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      pipeline.RasterizerState.DepthClipEnable = TRUE;
      pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      pipeline.NumRenderTargets = 1;
      pipeline.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
      pipeline.SampleDesc.Count = 1;
      pipeline.PS = {kDepthResolveSinglePs, sizeof(kDepthResolveSinglePs)};
      if (FAILED(hr = device->d3d->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&state_->depth_resolve_single))))
        return fail("native-depth-resolve-pipeline", hr);
      pipeline.PS = {kDepthResolveMsPs, sizeof(kDepthResolveMsPs)};
      if (FAILED(hr = device->d3d->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&state_->depth_resolve_ms))))
        return fail("native-depth-resolve-pipeline", hr);
      pipeline.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
      pipeline.PS = {kSampleResolvePs, sizeof(kSampleResolvePs)};
      if (FAILED(hr = device->d3d->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&state_->sample_resolve))))
        return fail("native-sample-resolve-pipeline", hr);
      D3D12_DESCRIPTOR_HEAP_DESC rtv{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
      if (FAILED(hr = device->d3d->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&state_->depth_resolve_rtv))))
        return fail("native-depth-resolve-heaps", hr);
    }
    const char* layer_reason = nullptr;
    if (!state_->EnsureDrawLayer(layer_reason, hr)) return fail(layer_reason, hr);
    uint32_t slot = 0;
    if (!state_->AllocateDrawView(slot)) return fail("native-draw-views-exhausted", E_FAIL, state_->DrawViewCounts());
    state_->batch.views.push_back(slot);  // Free again once this batch completed.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    // The depth plane of D32_FLOAT_S8X24, or the color surface.
    srv.Format = depth ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (source.desc.samples > 1) {
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    } else {
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv.Texture2D.MipLevels = 1;
    }
    const UINT view_step = device->d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto view_cpu = state_->draw_views->GetCPUDescriptorHandleForHeapStart();
    view_cpu.ptr += SIZE_T(view_step) * slot;
    device->d3d->CreateShaderResourceView(src, &srv, view_cpu);
    const auto rtv = state_->depth_resolve_rtv->GetCPUDescriptorHandleForHeapStart();
    device->d3d->CreateRenderTargetView(dst, nullptr, rtv);
    barrier(src, source.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    barrier(dst, dest.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // A new destination is a render target never initialized: the pass writes
    // every texel, so it is discarded first.
    if (!dest.last_use_fence) commands->DiscardResource(dst, nullptr);
    // The draw layer restores all graphics state after the resolve pass.
    state_->batch.draw_bindings = false;
    commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, float(dest.width), float(dest.height), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, LONG(dest.width), LONG(dest.height)};
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    ID3D12DescriptorHeap* heaps[] = {state_->draw_views.Get(), state_->draw_samplers.Get()};
    commands->SetDescriptorHeaps(2, heaps);
    commands->SetGraphicsRootSignature(state_->depth_resolve_root.Get());
    commands->SetPipelineState(sampled ? state_->sample_resolve.Get() :
                               source.desc.samples > 1 ? state_->depth_resolve_ms.Get() :
                               state_->depth_resolve_single.Get());
    auto view_gpu = state_->draw_views->GetGPUDescriptorHandleForHeapStart();
    view_gpu.ptr += UINT64(view_step) * slot;
    commands->SetGraphicsRootDescriptorTable(0, view_gpu);
    // A guest sample the host count lacks (msaa_samples 2 on a 4X target)
    // wraps as the Xenos 2X does (k2 is k0, k3 is k1).
    const uint32_t sample = sampled ? uint32_t(request.sample) % source.desc.samples : uint32_t(request.sample);
    const uint32_t pass[2] = {sample, source.desc.samples};
    commands->SetGraphicsRoot32BitConstants(1, 2, pass, 0);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->DrawInstanced(3, 1, 0, 0);
    if (sampled) ++state_->sample_resolves;
    else ++state_->depth_resolves;
    method = sampled ? "sample-pass" : source.desc.samples > 1 ?
        (request.sample < 0 ? "depth-pass-min" : "depth-pass-sample") : "depth-pass-copy";
  } else if (source.desc.samples > 1 && same_size && !request.has_rect) {
    barrier(src, source.state, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    barrier(dst, dest.state, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    // With the half-pixel offset gap (regions.h) to fill, by regions.
    const auto copies = ResolveCopies(state_->render_mode.scale, {0, 0, dest.width, dest.height}, 0, 0);
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList1> regions;
    if (copies.size() > 1 && SUCCEEDED(commands->QueryInterface(IID_PPV_ARGS(&regions)))) {
      for (const GapCopy& g : copies) {
        D3D12_RECT rect{LONG(g.source.left), LONG(g.source.top), LONG(g.source.right), LONG(g.source.bottom)};
        regions->ResolveSubresourceRegion(dst, 0, g.dest_x, g.dest_y, src, 0, &rect, DXGI_FORMAT_R8G8B8A8_UNORM,
                                          D3D12_RESOLVE_MODE_AVERAGE);
      }
    } else {
      commands->ResolveSubresource(dst, 0, src, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    }
    method = "resolve-subresource";
  } else if (source.desc.samples == 1) {
    barrier(src, source.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(dst, dest.state, D3D12_RESOURCE_STATE_COPY_DEST);
    HostBox region;
    if (!CopyBox(host, request.has_rect, source.desc.width, source.desc.height, dest.width, dest.height, region))
      return fail("native-resolve-empty-region");
    D3D12_TEXTURE_COPY_LOCATION to{}, from{};
    to.pResource = dst; to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.pResource = src; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    // With the half-pixel offset gap (regions.h) filled.
    for (const GapCopy& g : ResolveCopies(state_->render_mode.scale, region, host.dest_x, host.dest_y)) {
      const D3D12_BOX box{g.source.left, g.source.top, 0, g.source.right, g.source.bottom, 1};
      commands->CopyTextureRegion(&to, g.dest_x, g.dest_y, 0, &from, &box);
    }
  } else {
    return fail("native-resolve-msaa-region-uncovered");  // Refused above, kept as a backstop.
  }
  source.last_use_fence = dest.last_use_fence = state_->batch.fence;
  dest.content_defined = true;
  fenceValue = state_->batch.fence;
  ++state_->batch.operations;
  ++state_->operations;
  evidence = fmt::format("{{\"resolved\":true,\"method\":\"{}\",\"fence\":{},\"source\":{},\"dest\":{},"
      "\"source_size\":[{},{}],\"dest_size\":[{},{}],\"samples\":{},\"operations\":{}}}", method, fenceValue,
      request.source_key, request.dest_key, source.desc.width, source.desc.height, dest.width, dest.height,
      source.desc.samples, state_->operations);
  return true;
}

namespace {
// Xenos blend factor and function (RB_BLENDCONTROL fields) to D3D12.
D3D12_BLEND xenosBlend(uint32_t f, bool& ok) {
  switch (f) {
    case 0: return D3D12_BLEND_ZERO;              case 1: return D3D12_BLEND_ONE;
    case 4: return D3D12_BLEND_SRC_COLOR;         case 5: return D3D12_BLEND_INV_SRC_COLOR;
    case 6: return D3D12_BLEND_SRC_ALPHA;         case 7: return D3D12_BLEND_INV_SRC_ALPHA;
    case 8: return D3D12_BLEND_DEST_COLOR;        case 9: return D3D12_BLEND_INV_DEST_COLOR;
    case 10: return D3D12_BLEND_DEST_ALPHA;       case 11: return D3D12_BLEND_INV_DEST_ALPHA;
    case 12: case 14: return D3D12_BLEND_BLEND_FACTOR;
    case 13: case 15: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 16: return D3D12_BLEND_SRC_ALPHA_SAT;
    default: ok = false; return D3D12_BLEND_ONE;
  }
}
D3D12_BLEND_OP xenosBlendOp(uint32_t f, bool& ok) {
  switch (f) {
    case 0: return D3D12_BLEND_OP_ADD;  case 1: return D3D12_BLEND_OP_SUBTRACT;
    case 2: return D3D12_BLEND_OP_MIN;  case 3: return D3D12_BLEND_OP_MAX;
    case 4: return D3D12_BLEND_OP_REV_SUBTRACT;
    default: ok = false; return D3D12_BLEND_OP_ADD;
  }
}
constexpr UINT kDrawViews = 4096, kDrawSamplers = 2048;
// A request's constants onto the engine's block: the whole block
// (4096 bytes) replaces it; otherwise the delta records (uint16 first
// register, uint16 count, count x 16 bytes) are applied in order. False: a
// malformed request.
bool ApplyConstants(std::span<const uint8_t> whole, std::span<const uint8_t> delta,
                    std::array<uint8_t, 4096>& block) {
  if (whole.size() == 4096) {
    std::memcpy(block.data(), whole.data(), 4096);
    return delta.empty();
  }
  if (!whole.empty()) return false;
  size_t at = 0;
  while (at + 4 <= delta.size()) {
    uint16_t first = 0, count = 0;
    std::memcpy(&first, &delta[at], 2);
    std::memcpy(&count, &delta[at + 2], 2);
    const size_t bytes = size_t(count) * 16;
    if (!count || size_t(first) + count > 256 || at + 4 + bytes > delta.size()) return false;
    std::memcpy(&block[size_t(first) * 16], &delta[at + 4], bytes);
    at += 4 + bytes;
  }
  return at == delta.size();
}
// The pipeline of DrawIndexed from the state words of its key (DrawStateWords,
// pipeline_keys.h), shared with PrecompilePipeline, so a precompiled pipeline
// is the one the draw would create. No PS for depth only.
HRESULT CreateDrawPipeline(ID3D12Device* device, ID3D12RootSignature* root, const std::array<uint32_t, 21>& w,
                           const std::vector<VertexElement>& elements, const std::vector<uint8_t>& vs,
                           const std::vector<uint8_t>* ps, Microsoft::WRL::ComPtr<ID3D12PipelineState>& out,
                           const char*& reason) {
  const uint32_t bc = w[0];
  const bool depth_only = w[17], has_color = w[18], has_depth = w[9];
  bool states_ok = true;
  D3D12_RENDER_TARGET_BLEND_DESC blend{};
  blend.SrcBlend = xenosBlend(bc & 0x1F, states_ok);
  blend.BlendOp = xenosBlendOp((bc >> 5) & 7, states_ok);
  blend.DestBlend = xenosBlend((bc >> 8) & 0x1F, states_ok);
  blend.SrcBlendAlpha = xenosBlend((bc >> 16) & 0x1F, states_ok);
  blend.BlendOpAlpha = xenosBlendOp((bc >> 21) & 7, states_ok);
  blend.DestBlendAlpha = xenosBlend((bc >> 24) & 0x1F, states_ok);
  // Alpha blend factors cannot use color terms in D3D12.
  auto alpha_factor = [](D3D12_BLEND b) {
    switch (b) {
      case D3D12_BLEND_SRC_COLOR: return D3D12_BLEND_SRC_ALPHA;
      case D3D12_BLEND_INV_SRC_COLOR: return D3D12_BLEND_INV_SRC_ALPHA;
      case D3D12_BLEND_DEST_COLOR: return D3D12_BLEND_DEST_ALPHA;
      case D3D12_BLEND_INV_DEST_COLOR: return D3D12_BLEND_INV_DEST_ALPHA;
      default: return b;
    }
  };
  blend.SrcBlendAlpha = alpha_factor(blend.SrcBlendAlpha);
  blend.DestBlendAlpha = alpha_factor(blend.DestBlendAlpha);
  blend.BlendEnable = !(bc == 0x00010001) && !depth_only;
  blend.RenderTargetWriteMask = depth_only ? 0 : UINT8(w[1] & 0xF);  // No color written.
  if (!states_ok) { reason = "native-draw-blend-uncovered"; return E_FAIL; }
  std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
  for (const auto& e : elements)
    layout.push_back({e.semantic, e.semantic_index, DxgiFormat(e.format), e.slot, e.offset,
                      D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0});
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
  d.pRootSignature = root;
  d.VS = {vs.data(), vs.size()};
  if (!depth_only && ps) d.PS = {ps->data(), ps->size()};  // None for depth only.
  d.BlendState.RenderTarget[0] = blend;
  d.BlendState.AlphaToCoverageEnable = w[11];
  d.SampleMask = UINT_MAX;
  d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  const bool cull_front = w[5], cull_back = w[6];
  d.RasterizerState.CullMode = cull_front && cull_back ? D3D12_CULL_MODE_NONE :
      cull_front ? D3D12_CULL_MODE_FRONT : cull_back ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
  d.RasterizerState.FrontCounterClockwise = w[7];
  d.RasterizerState.DepthBias = int32_t(w[19]);  // Polygon offset.
  d.RasterizerState.SlopeScaledDepthBias = std::bit_cast<float>(w[20]);
  d.RasterizerState.DepthClipEnable = TRUE;
  d.DepthStencilState.DepthEnable = w[2];
  d.DepthStencilState.DepthWriteMask = w[3] ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
  d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC(D3D12_COMPARISON_FUNC_NEVER + (w[4] & 7));
  // Xenos compare functions and stencil ops are the D3D12 values minus 1.
  auto face = [](uint32_t s) {
    return D3D12_DEPTH_STENCILOP_DESC{D3D12_STENCIL_OP(D3D12_STENCIL_OP_KEEP + ((s >> 3) & 7)),
                                      D3D12_STENCIL_OP(D3D12_STENCIL_OP_KEEP + ((s >> 9) & 7)),
                                      D3D12_STENCIL_OP(D3D12_STENCIL_OP_KEEP + ((s >> 6) & 7)),
                                      D3D12_COMPARISON_FUNC(D3D12_COMPARISON_FUNC_NEVER + (s & 7))};
  };
  d.DepthStencilState.StencilEnable = w[12];
  d.DepthStencilState.StencilReadMask = UINT8(w[15]);
  d.DepthStencilState.StencilWriteMask = UINT8(w[16]);
  d.DepthStencilState.FrontFace = face(w[13]);
  d.DepthStencilState.BackFace = face(w[14]);
  d.InputLayout = {layout.data(), UINT(layout.size())};
  d.PrimitiveTopologyType =
      w[10] == kTopologyClassTriangle ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE : D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
  d.NumRenderTargets = has_color ? 1 : 0;
  d.RTVFormats[0] = has_color ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_UNKNOWN;
  d.DSVFormat = has_depth ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_UNKNOWN;
  d.SampleDesc.Count = w[8];
  const HRESULT hr = device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&out));
  if (FAILED(hr)) reason = "native-draw-pipeline";
  return hr;
}
// The pipeline of CreateDrawPipeline as the opaque pipeline of the set
// (pipeline_set.h), which releases it; PipelineState reads it back.
NativeResult CreateSetPipeline(ID3D12Device* device, ID3D12RootSignature* root, const std::array<uint32_t, 21>& w,
                               const std::vector<VertexElement>& elements, const std::vector<uint8_t>& vs,
                               const std::vector<uint8_t>* ps, PipelineSet::Pipeline& out) {
  Microsoft::WRL::ComPtr<ID3D12PipelineState> created;
  const char* reason = nullptr;
  const HRESULT hr = CreateDrawPipeline(device, root, w, elements, vs, ps, created, reason);
  if (FAILED(hr)) return NativeResult::Failed(reason, hr);
  out = PipelineSet::Pipeline(created.Detach(),
                              [](void* state) { static_cast<ID3D12PipelineState*>(state)->Release(); });
  return {};
}
ID3D12PipelineState* PipelineState(const PipelineSet::Pipeline& pipeline) {
  return static_cast<ID3D12PipelineState*>(pipeline.get());
}
// DXIL binaries, the vertex shader always and the pixel shader unless the
// draw is depth only (then none).
bool DxilShaders(const ShaderRef& vs, const ShaderRef& ps, bool depth_only) {
  if (!vs.Present() || vs.format != ShaderFormat::kDxil) return false;
  return depth_only ? !ps.bytes : ps.Present() && ps.format == ShaderFormat::kDxil;
}
}  // namespace

// The draw root signature and heaps, created by the first draw or by
// PreparePipelines before the pool threads use the root signature.
bool Backend::State::AllocateDrawView(uint32_t& slot) {
  if (!free_views.empty()) { slot = free_views.back(); free_views.pop_back(); return true; }
  if (next_view >= kDrawViews) return false;
  slot = next_view++;
  return true;
}

std::string Backend::State::DrawViewCounts() const {
  return fmt::format("\"views\":{{\"capacity\":{},\"allocated\":{},\"free\":{},\"sampled\":{},\"retired_sampled\":{},"
                     "\"sampled_released\":{},\"in_batch\":{}}}", kDrawViews, next_view - 1, free_views.size(),
                     sampled.size(), retired_sampled.size(), sampled_released, batch.views.size());
}

bool Backend::State::EnsureDrawLayer(const char*& reason, HRESULT& hr) {
  if (draw_root) return true;
  auto* device = static_cast<plume::D3D12Device*>(this->device.get());
  // XenosRecomp layout: unbounded texture tables in spaces 0..2, samplers in
  // space 3, root CBVs b0..b2 in space 4.
  D3D12_DESCRIPTOR_RANGE ranges[4] = {
      {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 1, 0},
      {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 2, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, UINT_MAX, 0, 3, 0}};
  D3D12_ROOT_PARAMETER params[7]{};
  for (unsigned i = 0; i < 4; ++i) {
    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[i].DescriptorTable = {1, &ranges[i]};
    params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  for (unsigned i = 0; i < 3; ++i) {
    params[4 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[4 + i].Descriptor = {i, 4};
    params[4 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC desc{7, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  Microsoft::WRL::ComPtr<ID3DBlob> blob, error;
  hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
  if (FAILED(hr)) { reason = "native-draw-root-serialize"; return false; }
  hr = device->d3d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&draw_root));
  if (FAILED(hr)) { reason = "native-draw-root-create"; return false; }
  D3D12_DESCRIPTOR_HEAP_DESC views{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kDrawViews, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  D3D12_DESCRIPTOR_HEAP_DESC samplers{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kDrawSamplers, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (FAILED(hr = device->d3d->CreateDescriptorHeap(&views, IID_PPV_ARGS(&draw_views))) ||
      FAILED(hr = device->d3d->CreateDescriptorHeap(&samplers, IID_PPV_ARGS(&draw_samplers)))) {
    draw_root.Reset();
    reason = "native-draw-heaps";
    return false;
  }
  // Every slot starts with a null 2D view and a point sampler; sampled
  // textures and their samplers take fixed slots from 1 on.
  const UINT view_step = device->d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const UINT sampler_step = device->d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  D3D12_SHADER_RESOURCE_VIEW_DESC null_view{};
  null_view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  null_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  null_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  null_view.Texture2D.MipLevels = 1;
  D3D12_SAMPLER_DESC point{};
  point.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  point.AddressU = point.AddressV = point.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  point.MaxLOD = D3D12_FLOAT32_MAX;
  auto view = draw_views->GetCPUDescriptorHandleForHeapStart();
  auto sampler = draw_samplers->GetCPUDescriptorHandleForHeapStart();
  for (unsigned i = 0; i < kDrawViews; ++i, view.ptr += view_step) device->d3d->CreateShaderResourceView(nullptr, &null_view, view);
  for (unsigned i = 0; i < kDrawSamplers; ++i, sampler.ptr += sampler_step) device->d3d->CreateSampler(&point, sampler);
  // The pipeline set creates through the draw layer, on any thread: the device
  // is free threaded and the root signature is read only from here on.
  ID3D12Device* const d3d = device->d3d;
  ID3D12RootSignature* const root = draw_root.Get();
  pipeline_set.Configure([d3d, root](const PipelineDesc& d, PipelineSet::Pipeline& out) {
    return CreateSetPipeline(d3d, root, d.state, *d.elements, *d.vs.bytes, d.ps.bytes.get(), out);
  });
  return true;
}

bool Backend::DrawIndexed(const DrawRequest& r, uint64_t& fenceValue, std::string& evidence) {
  profile::Timer profile(profile::kEngineDraw);
  fenceValue = 0;
  auto fail = [&](const char* reason, HRESULT hr = E_FAIL, const std::string& fields = {}) {
    evidence = fmt::format("{{\"drawn\":false,\"reason\":\"{}\",\"hresult\":{}{}{}}}", reason,
                           static_cast<uint32_t>(hr), fields.empty() ? "" : ",", fields);
    return false;
  };
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId() || state_->operation_poisoned ||
      state_->unproved_execution) return fail("native-draw-owner-or-state");
  // A depth-only draw may have no color target; the depth target then
  // gives the size and the samples.
  State::Surface* color = nullptr;
  if (!r.depth_only || r.color_key) {
    auto color_it = state_->surfaces.find(r.color_key);
    if (color_it == state_->surfaces.end() || color_it->second.desc.format != SurfaceFormat::Color8888)
      return fail("native-draw-color-surface-unknown");
    color = &color_it->second;
  }
  State::Surface* depth = nullptr;
  if (r.depth_enable || r.stencil_enable) {
    auto it = state_->surfaces.find(r.depth_key);
    if (it == state_->surfaces.end() || it->second.desc.format != SurfaceFormat::DepthStencil ||
        (color && (it->second.desc.samples != color->desc.samples || it->second.desc.width < color->desc.width ||
                   it->second.desc.height < color->desc.height))) return fail("native-draw-depth-surface");
    depth = &it->second;
  }
  if (r.depth_only && !depth) return fail("native-draw-depth-only-without-depth");
  const State::Surface& target = color ? *color : *depth;
  if (!DxilShaders(r.vs, r.ps, r.depth_only)) return fail("native-draw-shaders");
  // Indices and streams either inline in the request or views of the
  // buffers the engine keeps (UploadBuffer), bound whole.
  const size_t index_bytes = size_t(r.index_count) * (r.index32 ? 4 : 2);
  if (!r.index_count) return fail("native-draw-indices");
  State::Buffer* index_buffer = nullptr;
  if (r.index_key) {
    auto it = state_->buffers.find(r.index_key);
    if (it == state_->buffers.end() || !it->second.resource || r.index_offset > it->second.bytes ||
        index_bytes > it->second.bytes - r.index_offset) return fail("native-draw-index-buffer");
    index_buffer = &it->second;
  } else if (r.indices.size() < index_bytes) {
    return fail("native-draw-indices");
  }
  if (r.streams.empty() || r.streams.size() > kDrawStreamsMax || !r.elements || r.elements->empty())
    return fail("native-draw-vertex-input");
  const std::vector<VertexElement>& elements = *r.elements;
  std::array<State::Buffer*, kDrawStreamsMax> stream_buffers{};
  for (size_t i = 0; i < r.streams.size(); ++i) {
    const auto& s = r.streams[i];
    if (!s.key) continue;
    auto it = state_->buffers.find(s.key);
    if (it == state_->buffers.end() || !it->second.resource || !s.bytes || s.offset > it->second.bytes ||
        s.bytes > it->second.bytes - s.offset) return fail("native-draw-stream-buffer");
    stream_buffers[i] = &it->second;
  }
  for (const auto& e : elements)
    if (e.slot >= r.streams.size() || !r.streams[e.slot].stride ||
        (!stream_buffers[e.slot] && r.streams[e.slot].data.empty()))
      return fail("native-draw-vertex-element");
  auto* device = static_cast<plume::D3D12Device*>(state_->device.get());
  auto* fence = static_cast<plume::D3D12CommandFence*>(state_->fence.get());
  HRESULT hr = S_OK;
  {
    const char* reason = nullptr;
    if (!state_->EnsureDrawLayer(reason, hr)) return fail(reason, hr);
  }
  // Stable identities only: the shader names of the cache (with the
  // policy or spec suffix), the layout id of the pair (VS, declaration) and
  // the state words of the draw's state snapshot plus the target (samples,
  // depth and color presence). The same key on every boot, so the set of a
  // scene can be listed (PipelineList) and precompiled.
  const uint32_t bc = r.blend_control;
  const uint64_t layout_id = r.layout_id ? r.layout_id : ElementsId(elements);
  std::array<uint32_t, 21> state_words{};
  if (!DrawStateWords(r, target.desc.samples, depth != nullptr, color != nullptr, state_words))
    return fail("native-draw-topology-uncovered");
  const uint64_t key = PipelineKey(r.vs.key, r.ps.key, layout_id, state_words);
  auto& pipeline = state_->pipelines[key];
  const bool pipeline_created = !pipeline;
  bool skip_draw = false;  // The pipeline is being created asynchronously (skip take).
  if (!pipeline) {
    profile::Timer pipeline_time(profile::kEnginePipeline);
    // The take (pipeline_set.h): a ready pipeline is taken; one a pool or an
    // asynchronous thread is creating is waited for; one still queued is taken
    // back; otherwise the draw creates it. A batched draw found its absent
    // pipeline posted before the batch (PrefetchPipelines). The skip take
    // (REX_NATIVE_PSO_ASYNC=1, diagnostics) posts a pipeline not ready and
    // records the draw without itself.
    const uint64_t triple = TripleKey(r.vs.key, r.ps.key, layout_id);
    PipelineSet::Taken take = state_->pipeline_set.Take(key, triple, r.pipeline_take);
    const char* origin = take.origin;
    pipeline = std::move(take.pipeline);
    skip_draw = take.skip;
    uint32_t create_us = 0;
    if (take.post) {
      state_->pipeline_set.Post(key, PipelineDesc{r.vs, r.ps, r.elements, layout_id, state_words},
          false, true);
    }
    if (!skip_draw && !origin) {
      const auto started = std::chrono::steady_clock::now();
      const NativeResult created = CreateSetPipeline(device->d3d, state_->draw_root.Get(), state_words, elements,
                                                     *r.vs.bytes, r.depth_only ? nullptr : r.ps.bytes.get(), pipeline);
      if (!created.ok) {
        state_->pipelines.erase(key);
        return fail(created.reason, HRESULT(created.code));
      }
      create_us = uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - started).count());
      origin = state_->pipeline_set.NoteCreated(take, triple, create_us);
    }
    pipeline_time.Stop();
    if (skip_draw) state_->pipelines.erase(key);  // `pipeline` is not used below.
    else state_->pipeline_records.push_back({key, layout_id, r.vs.key, r.ps.key, r.elements, state_words, origin,
                                             create_us, WorldKey()});
  }
  // Textures: own copies with fixed SRV slots and cached samplers.
  // A copy is recreated only when no submitted work can still read it; a new
  // version is copied into the same resource after a transition barrier.
  const UINT view_step = device->d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const UINT sampler_step = device->d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  auto allocate_view = [&](uint32_t& slot) { return slot || state_->AllocateDrawView(slot); };
  // The draw records into the open batch. A copy or a view slot that
  // recorded or submitted work can still read is never rewritten: the open
  // batch retains it until its fence and the new copy takes a new slot.
  const char* batch_reason = nullptr;
  if (!state_->OpenBatch(batch_reason, hr)) return fail(batch_reason, hr);
  const uint64_t completed_now = fence->d3d->GetCompletedValue();
  auto in_use = [&](uint64_t last_use) {
    return last_use && (completed_now == UINT64_MAX || completed_now < last_use);
  };
  struct TextureUse {
    ID3D12Resource* resource;
    D3D12_RESOURCE_STATES* state;
    uint64_t* last_use;
    const DrawTexture* upload;  // Non-null: copy the levels first.
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    std::vector<UINT> rows;
    std::vector<UINT64> row_bytes;
    size_t offset = 0, bytes = 0;
  };
  std::vector<TextureUse> texture_uses;
  std::array<uint8_t, 512> shared = r.shared_constants;
  // Ps_param_gen gives guest pixels; the XenosRecomp fork multiplies the
  // host position by g_InverseResolutionScale (shared +280).
  const float inverse_scale[2] = {1.0f / state_->render_mode.scale.x, 1.0f / state_->render_mode.scale.y};
  std::memcpy(&shared[280], inverse_scale, sizeof(inverse_scale));
  std::string texture_record;
  for (const auto& t : r.textures) {
    // Registers 16 to 19 are the vertex textures (checked with their
    // shared-constant slots below); the bone matrices reach here.
    if (t.reg > 19) return fail("native-draw-texture-register");
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = ShaderComponentMapping(t.component_mapping);
    uint32_t view = 0;
    bool write_view = false, uploaded = false;
    TextureUse use{};
    if (t.resolved) {
      // A depth destination (R32_FLOAT of the depth resolve pass) is
      // sampled too; the view takes the format of the own texture.
      auto it = state_->textures.find(t.key);
      if (it == state_->textures.end() || !it->second.texture || !it->second.content_defined)
        return fail("native-draw-resolved-texture-undefined");
      auto& texture = it->second;
      use.resource = static_cast<plume::D3D12Texture*>(texture.texture.get())->d3d;
      if (texture.view && texture.view_of != use.resource && in_use(texture.last_use_fence)) {
        state_->batch.views.push_back(texture.view);
        texture.view = 0;
      }
      if (!allocate_view(texture.view)) return fail("native-draw-views-exhausted", E_FAIL, state_->DrawViewCounts());
      write_view = texture.view_of != use.resource;
      texture.view_of = use.resource;
      view = texture.view;
      use.state = &texture.state;
      use.last_use = &texture.last_use_fence;
      srv.Format = use.resource->GetDesc().Format;
      srv.Texture2D.MipLevels = 1;
    } else {
      auto& s = state_->sampled[t.key];
      const bool recreate = !s.resource || s.layout != t.layout || s.format != t.format || s.width != t.width ||
                            s.height != t.height || s.levels != t.levels || s.cube != t.cube;
      if (!t.width || !t.height || !t.levels || t.levels > 13 || t.width > 8192 || t.height > 8192)
        return fail("native-draw-texture-description");
      // A draw without data samples the copy UploadSampled made, at its
      // version or a later one.
      const uint32_t subresources = t.levels * (t.cube ? 6 : 1);
      const bool has_data = t.data.size() == subresources;
      if (!has_data && (recreate || s.version < t.version)) return fail("native-draw-texture-data-missing");
      if (recreate) {
        profile::Timer texture_time(profile::kEngineTexture);
        if (s.resource && in_use(s.last_use_fence)) {
          state_->batch.retained.push_back(s.resource);
          if (s.view) state_->batch.views.push_back(s.view);
          s.view = 0;
        }
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = t.width;
        desc.Height = t.height;
        desc.DepthOrArraySize = t.cube ? 6 : 1;
        desc.MipLevels = UINT16(t.levels);
        desc.Format = DxgiFormat(t.format);
        desc.SampleDesc.Count = 1;
        s.resource.Reset();
        hr = device->d3d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                  nullptr, IID_PPV_ARGS(&s.resource));
        if (FAILED(hr)) { state_->sampled.erase(t.key); return fail("native-draw-texture-create", hr); }
        s.state = D3D12_RESOURCE_STATE_COPY_DEST;
        s.format = t.format; s.width = t.width; s.height = t.height; s.levels = t.levels; s.layout = t.layout;
        s.cube = t.cube;
        s.last_use_fence = 0;
        s.view_of = nullptr;
      }
      if (!allocate_view(s.view)) return fail("native-draw-views-exhausted", E_FAIL, state_->DrawViewCounts());
      use.resource = s.resource.Get();
      use.state = &s.state;
      use.last_use = &s.last_use_fence;
      write_view = s.view_of != use.resource;
      s.view_of = use.resource;
      if (has_data && (recreate || s.version < t.version)) {
        const auto desc = use.resource->GetDesc();
        use.footprints.resize(subresources);
        use.rows.resize(subresources);
        use.row_bytes.resize(subresources);
        UINT64 bytes = 0;
        device->d3d->GetCopyableFootprints(&desc, 0, subresources, 0, use.footprints.data(), use.rows.data(),
                                           use.row_bytes.data(), &bytes);
        for (uint32_t i = 0; i < subresources; ++i)
          if (t.data[i].size() != size_t(use.rows[i]) * use.row_bytes[i])
            return fail("native-draw-texture-data-size");
        use.upload = &t;
        use.bytes = size_t(bytes);
        s.version = t.version;
        uploaded = true;
      }
      view = s.view;
      srv.Format = DxgiFormat(t.format);
      if (t.cube) {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srv.TextureCube.MipLevels = t.levels;
      } else {
        srv.Texture2D.MipLevels = t.levels;
      }
    }
    if (write_view) {
      auto handle = state_->draw_views->GetCPUDescriptorHandleForHeapStart();
      handle.ptr += SIZE_T(view) * view_step;
      device->d3d->CreateShaderResourceView(use.resource, &srv, handle);
    }
    D3D12_SAMPLER_DESC sampler{};
    sampler.Filter = SamplerFilter(t);
    sampler.AddressU = TextureAddressMode(t.address_u);
    sampler.AddressV = TextureAddressMode(t.address_v);
    sampler.AddressW = TextureAddressMode(t.address_w);
    sampler.MipLODBias = t.lod_bias;
    sampler.MaxAnisotropy = t.max_anisotropy;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MinLOD = t.min_lod;
    sampler.MaxLOD = t.max_lod;
    for (unsigned i = 0; i < 4; ++i) sampler.BorderColor[i] = t.border_color[i];
    const uint64_t sampler_key = Fnv64(reinterpret_cast<const uint8_t*>(&sampler), sizeof(sampler), 0xCBF29CE484222325ull);
    auto slot_it = state_->sampler_slots.find(sampler_key);
    const bool inserted = slot_it == state_->sampler_slots.end();
    if (inserted) {
      if (state_->sampler_slot_use.empty()) {
        state_->sampler_slot_keys.assign(kDrawSamplers, 0);
        state_->sampler_slot_use.assign(kDrawSamplers, 0);
      }
      uint32_t slot = uint32_t(state_->sampler_slots.size() + 1);
      if (slot >= kDrawSamplers) {
        // Full: the slot of a sampler no recorded or submitted work can read.
        slot = 0;
        for (uint32_t i = 1; i < kDrawSamplers && !slot; ++i)
          if (!in_use(state_->sampler_slot_use[i])) slot = i;
        if (!slot) return fail("native-draw-samplers-exhausted");
        state_->sampler_slots.erase(state_->sampler_slot_keys[slot]);
      }
      slot_it = state_->sampler_slots.emplace(sampler_key, slot).first;
      state_->sampler_slot_keys[slot] = sampler_key;
      auto handle = state_->draw_samplers->GetCPUDescriptorHandleForHeapStart();
      handle.ptr += SIZE_T(slot) * sampler_step;
      device->d3d->CreateSampler(&sampler, handle);
    }
    const uint32_t sampler_slot = slot_it->second;
    state_->sampler_slot_use[sampler_slot] = state_->batch.fence;
    // Registers 0 to 15 at c0..c3 (view) and c12..c15 (sampler); the
    // vertex texture registers 16 to 19 at c18 and c19 (bytes 288 and 304),
    // as build-sdb-shader-cache.py declares them.
    if (t.reg > 19) return fail("native-draw-texture-register");
    // A cubemap's view index at +128 + 4r, read by tfetchCube.
    const size_t view_at = t.reg < 16 ? (t.cube ? 128 : 0) + 4 * t.reg : 288 + 4 * (t.reg - 16);
    if (t.cube && t.reg >= 16) return fail("native-draw-texture-register");
    const size_t sampler_at = t.reg < 16 ? 192 + 4 * t.reg : 304 + 4 * (t.reg - 16);
    std::memcpy(&shared[view_at], &view, 4);
    std::memcpy(&shared[sampler_at], &sampler_slot, 4);
    if (r.want_evidence)
      texture_record += fmt::format("{}{{\"reg\":{},\"key\":{},\"resolved\":{},\"view\":{},\"view_written\":{},"
          "\"sampler\":{},\"sampler_created\":{},\"uploaded\":{},\"format\":{},\"size\":[{},{}],\"levels\":{},"
          "\"version\":{}}}", texture_record.empty() ? "" : ",", t.reg, t.key, t.resolved, view, write_view,
          sampler_slot, inserted, uploaded, uint32_t(t.format), t.width, t.height, t.levels, t.version);
    texture_uses.push_back(std::move(use));
  }
  // One upload buffer per draw: streams, indices, the three constant blocks
  // and the texture levels to copy.
  auto align = [](size_t v) { return (v + 255) & ~size_t(255); };
  std::array<size_t, kDrawStreamsMax> stream_offsets{};
  size_t total = 0;
  for (size_t i = 0; i < r.streams.size(); ++i) {
    stream_offsets[i] = total;
    if (!r.streams[i].key) total = align(total + r.streams[i].data.size());
  }
  const size_t index_offset = total;
  if (!index_buffer) total = align(total + index_bytes);
  // The request's constants onto the blocks kept here (whole block or
  // the registers written since the previous request), then the usual reuse
  // of the CBV while the version holds within the batch, and of the shared
  // block's while its bytes do.
  if (!ApplyConstants(r.vs_constants, r.vs_constants_delta, state_->vs_block) ||
      !ApplyConstants(r.ps_constants, r.ps_constants_delta, state_->ps_block))
    return fail("native-draw-constants");
  profile::Count(profile::kConstantBytes, r.vs_constants.size() + r.ps_constants.size() +
                                                         r.vs_constants_delta.size() + r.ps_constants_delta.size());
  const bool vs_reuse = r.vs_constants_version && state_->cbv_batch_fence == state_->batch.fence &&
                        state_->last_vs_version == r.vs_constants_version && state_->last_vs_cbv;
  const bool ps_reuse = r.ps_constants_version && state_->cbv_batch_fence == state_->batch.fence &&
                        state_->last_ps_version == r.ps_constants_version && state_->last_ps_cbv;
  const bool shared_reuse = state_->cbv_batch_fence == state_->batch.fence && state_->last_shared_cbv &&
                            state_->last_shared == shared;
  const size_t vs_offset = total; if (!vs_reuse) total += 4096;
  const size_t ps_offset = total; if (!ps_reuse) total += 4096;
  const size_t shared_offset = total; if (!shared_reuse) total += 512;
  for (auto& use : texture_uses) {
    if (!use.upload) continue;
    total = (total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~size_t(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
    use.offset = total;
    total += use.bytes;
  }
  profile::Timer upload_time(profile::kEngineUpload);
  profile::Count(profile::kUploadBytes, total);
  State::UploadPage* page = nullptr;
  size_t page_offset = 0;
  if (!state_->Upload(total, page, page_offset, hr)) return fail("native-draw-upload", hr);
  uint8_t* const mapped = page->cpu + page_offset;
  ID3D12Resource* const upload = page->buffer.Get();
  for (size_t i = 0; i < r.streams.size(); ++i)
    if (!r.streams[i].key) std::memcpy(mapped + stream_offsets[i], r.streams[i].data.data(), r.streams[i].data.size());
  if (!index_buffer) std::memcpy(mapped + index_offset, r.indices.data(), index_bytes);
  if (!vs_reuse) std::memcpy(mapped + vs_offset, state_->vs_block.data(), 4096);
  if (!ps_reuse) std::memcpy(mapped + ps_offset, state_->ps_block.data(), 4096);
  if (!shared_reuse) std::memcpy(mapped + shared_offset, shared.data(), 512);
  for (const auto& use : texture_uses) {
    if (!use.upload) continue;
    for (size_t level = 0; level < use.footprints.size(); ++level) {
      const auto& f = use.footprints[level];
      const auto& data = use.upload->data[level];
      for (UINT row = 0; row < use.rows[level]; ++row)
        std::memcpy(mapped + use.offset + f.Offset + size_t(row) * f.Footprint.RowPitch,
                    data.data() + size_t(row) * use.row_bytes[level], size_t(use.row_bytes[level]));
    }
  }
  upload_time.Stop();
  profile::Timer record_time(profile::kEngineRecord);
  const D3D12_GPU_VIRTUAL_ADDRESS gpu = page->gpu + page_offset;
  auto& list = state_->batch_list;
  auto& bound = state_->batch.bound;
  const bool bindings_valid = state_->batch.draw_bindings;
  if (!bindings_valid) {
    bound.pipeline = nullptr;
    bound.stencil_valid = false;
  }
  // A skipped asynchronous draw does not bind a PSO. Keep the actually bound
  // one (or unknown after invalidation), so the next real draw restores it.
  if (!skip_draw && bound.pipeline != PipelineState(pipeline)) {
    list->SetPipelineState(PipelineState(pipeline));
    bound.pipeline = PipelineState(pipeline);
  }
  auto transition = [&](State::Surface& surface, D3D12_RESOURCE_STATES after) {
    if (surface.state == after) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = static_cast<plume::D3D12Texture*>(surface.texture.get())->d3d;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = surface.state;
    b.Transition.StateAfter = after;
    list->ResourceBarrier(1, &b);
    surface.state = after;
  };
  auto barrier = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES after) {
    if (state == after) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = state;
    b.Transition.StateAfter = after;
    list->ResourceBarrier(1, &b);
    state = after;
  };
  for (const auto& use : texture_uses) {
    if (use.upload) {
      barrier(use.resource, *use.state, D3D12_RESOURCE_STATE_COPY_DEST);
      for (size_t level = 0; level < use.footprints.size(); ++level) {
        D3D12_TEXTURE_COPY_LOCATION to{}, from{};
        to.pResource = use.resource;
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.SubresourceIndex = UINT(level);
        from.pResource = upload;
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint = use.footprints[level];
        from.PlacedFootprint.Offset += page_offset + use.offset;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      }
    }
    barrier(use.resource, *use.state, state_->shader_read);
  }
  if (color) transition(*color, D3D12_RESOURCE_STATE_RENDER_TARGET);
  if (depth) transition(*depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
  const auto rtv = color ? color->view->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE{};
  const auto dsv = depth ? depth->view->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE{};
  auto* color_resource = color ? static_cast<plume::D3D12Texture*>(color->texture.get())->d3d : nullptr;
  auto* depth_resource = depth ? static_cast<plume::D3D12Texture*>(depth->texture.get())->d3d : nullptr;
  if (!bindings_valid || bound.color != color_resource || bound.depth != depth_resource ||
      bound.rtv.ptr != rtv.ptr || bound.dsv.ptr != dsv.ptr) {
    list->OMSetRenderTargets(color ? 1 : 0, color ? &rtv : nullptr, FALSE, depth ? &dsv : nullptr);
    bound.color = color_resource;
    bound.depth = depth_resource;
    bound.rtv = rtv;
    bound.dsv = dsv;
  }
  // The viewport and the scissor come in guest pixels (regions.h).
  const auto& k = state_->render_mode.scale;
  const HostViewport host = ViewportInHost(k, r.viewport);
  const D3D12_VIEWPORT viewport{host.x, host.y, host.width, host.height, r.viewport[4], r.viewport[5]};
  if (!bindings_valid || std::memcmp(&bound.viewport, &viewport, sizeof(viewport))) {
    list->RSSetViewports(1, &viewport);
    bound.viewport = viewport;
  }
  D3D12_RECT scissor{std::max<LONG>(k.X(r.scissor[0]), 0), std::max<LONG>(k.Y(r.scissor[1]), 0),
                     std::min<LONG>(k.X(r.scissor[2]), target.desc.width),
                     std::min<LONG>(k.Y(r.scissor[3]), target.desc.height)};
  if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) scissor = {0, 0, 0, 0};
  if (!bindings_valid || std::memcmp(&bound.scissor, &scissor, sizeof(scissor))) {
    list->RSSetScissorRects(1, &scissor);
    bound.scissor = scissor;
  }
  if (!bindings_valid) {
    list->SetGraphicsRootSignature(state_->draw_root.Get());
    ID3D12DescriptorHeap* heaps[] = {state_->draw_views.Get(), state_->draw_samplers.Get()};
    list->SetDescriptorHeaps(2, heaps);
    for (unsigned i = 0; i < 3; ++i) list->SetGraphicsRootDescriptorTable(i, state_->draw_views->GetGPUDescriptorHandleForHeapStart());
    list->SetGraphicsRootDescriptorTable(3, state_->draw_samplers->GetGPUDescriptorHandleForHeapStart());
    state_->batch.draw_bindings = true;
  }
  const D3D12_GPU_VIRTUAL_ADDRESS vs_cbv = vs_reuse ? state_->last_vs_cbv : gpu + vs_offset;
  const D3D12_GPU_VIRTUAL_ADDRESS ps_cbv = ps_reuse ? state_->last_ps_cbv : gpu + ps_offset;
  const D3D12_GPU_VIRTUAL_ADDRESS shared_cbv = shared_reuse ? state_->last_shared_cbv : gpu + shared_offset;
  const std::array<D3D12_GPU_VIRTUAL_ADDRESS, 3> constants{vs_cbv, ps_cbv, shared_cbv};
  for (UINT i = 0; i < constants.size(); ++i) {
    if (!bindings_valid || bound.constants[i] != constants[i]) {
      list->SetGraphicsRootConstantBufferView(4 + i, constants[i]);
      bound.constants[i] = constants[i];
    }
  }
  state_->cbv_batch_fence = state_->batch.fence;
  state_->last_vs_cbv = vs_cbv;
  state_->last_ps_cbv = ps_cbv;
  if (!shared_reuse) {
    state_->last_shared_cbv = shared_cbv;
    state_->last_shared = shared;
  }
  state_->last_vs_version = r.vs_constants_version;
  state_->last_ps_version = r.ps_constants_version;
  std::array<D3D12_VERTEX_BUFFER_VIEW, kDrawStreamsMax> streams{};
  for (size_t i = 0; i < r.streams.size(); ++i) {
    if (State::Buffer* b = stream_buffers[i]) {
      barrier(b->resource.Get(), b->state, kBufferRead);
      streams[i] = {b->resource->GetGPUVirtualAddress() + r.streams[i].offset, r.streams[i].bytes, r.streams[i].stride};
    } else {
      streams[i] = {gpu + stream_offsets[i], UINT(r.streams[i].data.size()), r.streams[i].stride};
    }
  }
  const UINT stream_count = UINT(r.streams.size());
  if (!bindings_valid || bound.stream_count != stream_count ||
      std::memcmp(bound.streams.data(), streams.data(), stream_count * sizeof(streams[0]))) {
    list->IASetVertexBuffers(0, stream_count, streams.data());
    bound.streams = streams;
    bound.stream_count = stream_count;
  }
  if (index_buffer) barrier(index_buffer->resource.Get(), index_buffer->state, kBufferRead);
  const D3D12_INDEX_BUFFER_VIEW indices{
      index_buffer ? index_buffer->resource->GetGPUVirtualAddress() + r.index_offset : gpu + index_offset,
      UINT(index_bytes), r.index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT};
  if (!bindings_valid || std::memcmp(&bound.indices, &indices, sizeof(indices))) {
    list->IASetIndexBuffer(&indices);
    bound.indices = indices;
  }
  const auto topology = PrimitiveTopology(r.topology);
  if (!bindings_valid || bound.topology != topology) {
    list->IASetPrimitiveTopology(topology);
    bound.topology = topology;
  }
  if (!bindings_valid || std::memcmp(bound.blend.data(), r.blend_factor.data(), sizeof(bound.blend))) {
    list->OMSetBlendFactor(r.blend_factor.data());
    bound.blend = r.blend_factor;
  }
  if (r.stencil_enable && (!bound.stencil_valid || bound.stencil_ref != r.stencil_ref)) {
    list->OMSetStencilRef(r.stencil_ref);
    bound.stencil_ref = r.stencil_ref;
    bound.stencil_valid = true;
  }
  if (!skip_draw) list->DrawIndexedInstanced(r.index_count, 1, 0, r.base_vertex, 0);
  record_time.Stop();
  // Complete when the batch fence completes; no Signal of its own.
  const uint64_t batch_fence = state_->batch.fence;
  if (color) color->last_use_fence = batch_fence;
  if (depth) depth->last_use_fence = batch_fence;
  for (const auto& use : texture_uses) *use.last_use = batch_fence;
  for (State::Buffer* b : stream_buffers) if (b) b->last_use_fence = batch_fence;
  if (index_buffer) index_buffer->last_use_fence = batch_fence;
  fenceValue = batch_fence;
  const uint64_t batch_operations = ++state_->batch.operations;
  ++state_->operations;
  ++state_->draws;
  // A triangle list right after a draw of the batch that bound the same
  // state could have been part of it (profile::kDrawsSameState and below).
  if (Diagnostics()) {
    auto& p = state_->fusion_probe;
    const bool uploads = std::any_of(texture_uses.begin(), texture_uses.end(), [](const auto& u) { return u.upload; });
    const bool same_state = !skip_draw && !uploads && bindings_valid && p.fence == batch_fence &&
        p.operation + 1 == batch_operations && topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST &&
        p.bound.topology == topology && p.bound.pipeline == bound.pipeline && p.bound.color == bound.color &&
        p.bound.depth == bound.depth && p.bound.rtv.ptr == bound.rtv.ptr && p.bound.dsv.ptr == bound.dsv.ptr &&
        !std::memcmp(&p.bound.viewport, &bound.viewport, sizeof(bound.viewport)) &&
        !std::memcmp(&p.bound.scissor, &bound.scissor, sizeof(bound.scissor)) &&
        p.bound.constants[0] == bound.constants[0] && p.bound.constants[1] == bound.constants[1] &&
        p.shared == shared && p.bound.blend == bound.blend && p.bound.stencil_ref == bound.stencil_ref;
    const bool same_streams = same_state && p.bound.stream_count == bound.stream_count &&
        !std::memcmp(p.bound.streams.data(), bound.streams.data(), bound.stream_count * sizeof(bound.streams[0]));
    const bool fusable = same_streams && p.base_vertex == r.base_vertex &&
        p.bound.indices.Format == bound.indices.Format &&
        p.bound.indices.BufferLocation + p.bound.indices.SizeInBytes == bound.indices.BufferLocation;
    profile::Count(profile::kDrawsSameState, same_state);
    profile::Count(profile::kDrawsSameStreams, same_streams);
    profile::Count(profile::kDrawsFusable, fusable);
    p.fence = skip_draw ? 0 : batch_fence;
    p.operation = batch_operations;
    p.bound = bound;
    p.shared = shared;
    p.base_vertex = r.base_vertex;
  }
  if (batch_operations >= kBatchOperationsMax && !state_->CloseBatch(batch_reason, hr)) return fail(batch_reason, hr);
  if (!r.want_evidence) {  // A batched draw formats nothing.
    evidence.clear();
    return true;
  }
  evidence = fmt::format("{{\"drawn\":true,\"fence\":{},\"draws\":{},\"pipeline\":\"{:016x}\",\"pipeline_created\":{},"
      "\"pipelines\":{},\"target\":{},\"target_size\":[{},{}],\"samples\":{},\"depth\":{},\"blend\":{},\"index_count\":{},"
      "\"upload_bytes\":{},\"viewport\":[{},{},{},{},{},{}],\"scissor\":[{},{},{},{}],\"textures\":[{}],"
      "\"sampled_copies\":{},\"views\":{},\"samplers\":{},\"operations\":{},\"stencil\":{},"
      "\"batch\":{{\"fence\":{},\"operations\":{},\"upload_offset\":{}}}}}", fenceValue,
      state_->draws, key, pipeline_created, state_->pipelines.size(), r.color_key, target.desc.width, target.desc.height,
      target.desc.samples, depth ? r.depth_key : 0, bc != 0x00010001 && !r.depth_only, r.index_count, total, r.viewport[0],
      r.viewport[1], r.viewport[2], r.viewport[3], r.viewport[4], r.viewport[5], scissor.left, scissor.top,
      scissor.right, scissor.bottom, texture_record, state_->sampled.size(), state_->next_view - 1,
      state_->sampler_slots.size(), state_->operations,
      r.stencil_enable ? fmt::format("{{\"front\":{},\"back\":{},\"ref\":{},\"read_mask\":{},\"write_mask\":{}}}",
                                     r.stencil_front, r.stencil_back, r.stencil_ref, r.stencil_read_mask,
                                     r.stencil_write_mask) : std::string("null"), batch_fence, batch_operations,
      page_offset);
  return true;
}

std::string Backend::PipelineList() {
  std::string out;
  for (const auto& p : state_->pipeline_records) {
    std::string elements;
    if (p.elements)
      for (const auto& e : *p.elements)
        elements += fmt::format("{}[\"{}\",{},{},{},{}]", elements.empty() ? "" : ",", e.semantic, e.semantic_index,
                                uint32_t(e.format), e.slot, e.offset);
    out += fmt::format("{}{{\"key\":\"{:016x}\",\"vs\":\"{}\",\"ps\":\"{}\",\"layout\":\"{:016x}\",\"elements\":[{}],"
                       "\"state\":[{}],\"origin\":\"{}\",\"create_us\":{},\"world\":\"{:08x}\"}}", out.empty() ? "" : ",",
                       p.key, p.vs, p.ps, p.layout_id, elements, fmt::join(p.state, ","), p.origin, p.create_us, p.world);
  }
  return "[" + out + "]";
}


std::vector<PipelineDesc> Backend::PipelineDescs() {
  std::vector<PipelineDesc> out;
  for (const auto& p : state_->pipeline_records)
    out.push_back({ShaderRef{p.vs, ShaderStage::kVertex}, ShaderRef{p.ps, ShaderStage::kPixel}, p.elements,
                   p.layout_id, p.state});
  return out;
}

bool Backend::PreparePipelines(std::string& evidence) {
  HRESULT hr = S_OK;
  const char* reason = "native-draw-owner-or-state";
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId() || state_->operation_poisoned ||
      state_->unproved_execution || !state_->EnsureDrawLayer(reason, hr)) {
    evidence = fmt::format("{{\"prepared\":false,\"reason\":\"{}\",\"hresult\":{}}}", reason, uint32_t(hr));
    return false;
  }
  evidence = "{\"prepared\":true}";
  return true;
}

bool Backend::PrecompilePipeline(const PipelineDesc& d, bool& present, std::string& evidence) {
  present = false;
  auto fail = [&](const char* reason, HRESULT hr = E_FAIL) {
    evidence = fmt::format("{{\"precompiled\":false,\"reason\":\"{}\",\"vs\":\"{}\",\"ps\":\"{}\",\"hresult\":{}}}",
                           reason, d.vs.key, d.ps.key, uint32_t(hr));
    return false;
  };
  // The root signature of PreparePipelines, read only from then on; the D3D12
  // device is free threaded. The caller keeps the engine alive (the pool of
  // the dispatcher is joined before its Shutdown).
  auto* device = static_cast<plume::D3D12Device*>(state_->device.get());
  if (!device || !state_->draw_root) return fail("precompile-not-prepared");
  const bool depth_only = d.state[17];
  if (!DxilShaders(d.vs, d.ps, depth_only) || !d.elements || d.elements->empty())
    return fail("precompile-description");
  // The list holds the samples of the boots it came from; a multisampled
  // target has the sample count of the render mode, as in the key of the draw.
  std::array<uint32_t, 21> state = d.state;
  state[8] = state_->PipelineSamples(state[8]);
  const uint64_t key = PipelineKey(d.vs.key, d.ps.key, d.layout_id, state);
  PipelineDesc listed = d;
  listed.state = state;
  const NativeResult result = state_->pipeline_set.Precompile(key, listed, present);
  if (present) return true;  // Compiling, ready or the owner's already.
  if (!result.ok) return fail(result.reason, HRESULT(result.code));
  profile::Count(profile::kPipelinesPrecompiled, 1);
  return true;
}

// Before a batch is drawn (owner): the key of each prefetch draw as
// DrawIndexed computes it; one neither created nor in the pipeline set is
// posted to the asynchronous threads, so the absent pipelines of the batch
// are created in parallel while the draws before them record.
void Backend::PrefetchPipelines(const std::vector<DrawRequest>& batch) {
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId() || !state_->draw_root) return;
  for (const auto& r : batch) {
    if (r.pipeline_take != DrawRequest::PipelineTake::kPrefetch || !DxilShaders(r.vs, r.ps, r.depth_only) ||
        !r.elements || r.elements->empty()) continue;
    const State::Surface* color = nullptr;
    const State::Surface* depth = nullptr;
    if (!r.depth_only || r.color_key) {
      auto it = state_->surfaces.find(r.color_key);
      if (it == state_->surfaces.end()) continue;
      color = &it->second;
    }
    if (r.depth_enable || r.stencil_enable) {
      auto it = state_->surfaces.find(r.depth_key);
      if (it == state_->surfaces.end()) continue;
      depth = &it->second;
    }
    if (!color && !depth) continue;
    const uint64_t layout_id = r.layout_id ? r.layout_id : ElementsId(*r.elements);
    std::array<uint32_t, 21> words{};
    if (!DrawStateWords(r, (color ? color : depth)->desc.samples, depth != nullptr, color != nullptr, words)) continue;
    const uint64_t key = PipelineKey(r.vs.key, r.ps.key, layout_id, words);
    if (auto it = state_->pipelines.find(key); it != state_->pipelines.end() && it->second) continue;
    if (state_->pipeline_set.Post(key, PipelineDesc{r.vs, r.ps, r.elements, layout_id, words},
            true, false))
      profile::Count(profile::kPipelinesPrefetched, 1);
  }
}

// As PrecompilePipeline reads the state: the root signature of PreparePipelines
// and the render mode, fixed from then on; the key with the samples of the
// render mode, as the draw computes it.
bool Backend::PostPipeline(const PipelineDesc& d) {
  if (!state_->device || !state_->draw_root) return false;
  const bool depth_only = d.state[17];
  if (!DxilShaders(d.vs, d.ps, depth_only) || !d.elements || d.elements->empty()) return false;
  PipelineDesc listed = d;
  listed.state[8] = state_->PipelineSamples(listed.state[8]);
  const uint64_t key = PipelineKey(listed.vs.key, listed.ps.key, listed.layout_id, listed.state);
  return state_->pipeline_set.Post(key, std::move(listed), true, false);
}

std::string Backend::PipelineSummary() { return state_->pipeline_set.Summary(); }

bool Backend::UploadSampled(const DrawTexture& t, std::string& evidence) {
  profile::Timer profile(profile::kEngineTexture);
  auto fail = [&](const char* reason, HRESULT hr = E_FAIL) {
    evidence = fmt::format("{{\"uploaded\":false,\"reason\":\"{}\",\"key\":{},\"hresult\":{}}}", reason, t.key,
                           static_cast<uint32_t>(hr));
    return false;
  };
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId() || state_->operation_poisoned ||
      state_->unproved_execution) return fail("native-upload-owner-or-state");
  const uint32_t subresources = t.levels * (t.cube ? 6 : 1);
  if (t.resolved || !t.width || !t.height || !t.levels || t.levels > 13 || t.width > 8192 || t.height > 8192 ||
      t.data.size() != subresources) return fail("native-upload-texture-description");
  auto* device = static_cast<plume::D3D12Device*>(state_->device.get());
  auto* fence = static_cast<plume::D3D12CommandFence*>(state_->fence.get());
  HRESULT hr = S_OK;
  const char* batch_reason = nullptr;
  if (!state_->OpenBatch(batch_reason, hr)) return fail(batch_reason, hr);
  const uint64_t completed_now = fence->d3d->GetCompletedValue();
  auto in_use = [&](uint64_t last_use) {
    return last_use && (completed_now == UINT64_MAX || completed_now < last_use);
  };
  auto& s = state_->sampled[t.key];
  const bool recreate = !s.resource || s.layout != t.layout || s.format != t.format || s.width != t.width ||
                        s.height != t.height || s.levels != t.levels || s.cube != t.cube;
  if (!recreate && s.version >= t.version) {
    evidence = fmt::format("{{\"uploaded\":false,\"skipped\":true,\"key\":{},\"version\":{},\"copy_version\":{}}}",
                           t.key, t.version, s.version);
    return true;
  }
  if (recreate) {  // As in DrawIndexed.
    if (s.resource && in_use(s.last_use_fence)) {
      state_->batch.retained.push_back(s.resource);
      if (s.view) state_->batch.views.push_back(s.view);
      s.view = 0;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = t.width;
    desc.Height = t.height;
    desc.DepthOrArraySize = t.cube ? 6 : 1;
    desc.MipLevels = UINT16(t.levels);
    desc.Format = DxgiFormat(t.format);
    desc.SampleDesc.Count = 1;
    s.resource.Reset();
    {
      profile::Timer create_time(profile::kEngineTextureCreate);
      hr = device->d3d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                nullptr, IID_PPV_ARGS(&s.resource));
    }
    if (FAILED(hr)) { state_->sampled.erase(t.key); return fail("native-upload-texture-create", hr); }
    s.state = D3D12_RESOURCE_STATE_COPY_DEST;
    s.format = t.format; s.width = t.width; s.height = t.height; s.levels = t.levels; s.layout = t.layout;
    s.cube = t.cube;
    s.last_use_fence = 0;
    s.view_of = nullptr;
  }
  ID3D12Resource* const resource = s.resource.Get();
  const auto desc = resource->GetDesc();
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresources);
  std::vector<UINT> rows(subresources);
  std::vector<UINT64> row_bytes(subresources);
  UINT64 bytes = 0;
  device->d3d->GetCopyableFootprints(&desc, 0, subresources, 0, footprints.data(), rows.data(), row_bytes.data(), &bytes);
  for (uint32_t i = 0; i < subresources; ++i)
    if (t.data[i].size() != size_t(rows[i]) * row_bytes[i]) return fail("native-upload-texture-data-size");
  profile::Count(profile::kUploadBytes, bytes);
  State::UploadPage* page = nullptr;
  size_t page_offset = 0;
  if (!state_->Upload(size_t(bytes), page, page_offset, hr)) return fail("native-upload-buffer", hr);
  uint8_t* const mapped = page->cpu + page_offset;
  for (uint32_t i = 0; i < subresources; ++i)
    for (UINT row = 0; row < rows[i]; ++row)
      std::memcpy(mapped + footprints[i].Offset + size_t(row) * footprints[i].Footprint.RowPitch,
                  t.data[i].data() + size_t(row) * row_bytes[i], size_t(row_bytes[i]));
  auto& list = state_->batch_list;
  auto barrier = [&](D3D12_RESOURCE_STATES after) {
    if (s.state == after) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = s.state;
    b.Transition.StateAfter = after;
    list->ResourceBarrier(1, &b);
    s.state = after;
  };
  barrier(D3D12_RESOURCE_STATE_COPY_DEST);
  for (uint32_t level = 0; level < subresources; ++level) {
    D3D12_TEXTURE_COPY_LOCATION to{}, from{};
    to.pResource = resource;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.SubresourceIndex = UINT(level);
    from.pResource = page->buffer.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = footprints[level];
    from.PlacedFootprint.Offset += page_offset;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  }
  barrier(state_->shader_read);
  s.version = t.version;
  s.last_use_fence = state_->batch.fence;
  const uint64_t batch_operations = ++state_->batch.operations;
  ++state_->operations;
  if (batch_operations >= kBatchOperationsMax && !state_->CloseBatch(batch_reason, hr)) return fail(batch_reason, hr);
  evidence = fmt::format("{{\"uploaded\":true,\"key\":{},\"recreated\":{},\"version\":{},\"bytes\":{},"
                         "\"sampled_copies\":{}}}", t.key, recreate, t.version, bytes, state_->sampled.size());
  return true;
}

// A buffer of the draw layer (vertex or index data, little endian) in a
// default heap, created at its first upload and refreshed by ranges copied
// from the batch upload page, in the open batch before the draws that follow.
bool Backend::UploadBuffer(const BufferUpload& u, std::string& evidence) {
  profile::Timer profile(profile::kEngineBuffer);
  auto fail = [&](const char* reason, HRESULT hr = E_FAIL) {
    evidence = fmt::format("{{\"uploaded\":false,\"reason\":\"{}\",\"key\":{},\"hresult\":{}}}", reason, u.key,
                           static_cast<uint32_t>(hr));
    return false;
  };
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId() || state_->operation_poisoned ||
      state_->unproved_execution) return fail("native-upload-owner-or-state");
  if (!u.key || !u.bytes || u.ranges.empty() || u.bytes > (uint32_t(64) << 20)) return fail("native-upload-buffer-description");
  uint64_t range_bytes = 0;
  for (const auto& [offset, data] : u.ranges) {
    if (data.empty() || offset > u.bytes || data.size() > u.bytes - offset) return fail("native-upload-buffer-range");
    range_bytes += data.size();
  }
  auto* device = static_cast<plume::D3D12Device*>(state_->device.get());
  auto* fence = static_cast<plume::D3D12CommandFence*>(state_->fence.get());
  HRESULT hr = S_OK;
  const char* batch_reason = nullptr;
  if (!state_->OpenBatch(batch_reason, hr)) return fail(batch_reason, hr);
  const uint64_t completed_now = fence->d3d->GetCompletedValue();
  auto in_use = [&](uint64_t last_use) {
    return last_use && (completed_now == UINT64_MAX || completed_now < last_use);
  };
  auto& b = state_->buffers[u.key];
  const bool create = !b.resource || b.bytes != u.bytes;
  if (!create && b.version >= u.version) {
    evidence = fmt::format("{{\"uploaded\":false,\"skipped\":true,\"key\":{},\"version\":{},\"buffer_version\":{}}}",
                           u.key, u.version, b.version);
    return true;
  }
  if (create) {
    if (u.ranges.size() != 1 || u.ranges[0].first || u.ranges[0].second.size() != u.bytes) {
      if (!b.resource) state_->buffers.erase(u.key);
      return fail("native-upload-buffer-partial-create");
    }
    if (b.resource) {  // A grown buffer: the old one leaves at once or retired with its fence.
      if (in_use(b.last_use_fence)) {
        state_->retired_buffers.push_back(b);
        ++state_->buffers_retired;
      } else {
        b.resource.Reset();
        state_->FreeBufferBlocks(b);
        ++state_->buffers_released;
      }
      b.resource.Reset();
      b.blocks = 0;
    }
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = u.bytes;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    // A buffer of a default heap is created in COMMON (another initial state
    // is ignored, debug message 1328, which the frontend release refuses).
    const uint32_t blocks = (u.bytes + State::kBufferBlockBytes - 1) / State::kBufferBlockBytes;
    if (!state_->AllocateBufferBlocks(blocks, b.heap, b.block, hr)) {
      state_->buffers.erase(u.key);
      return fail("native-upload-buffer-heap", hr);
    }
    b.blocks = blocks;
    hr = device->d3d->CreatePlacedResource(state_->buffer_heaps[b.heap].heap.Get(),
                                           uint64_t(b.block) * State::kBufferBlockBytes, &desc,
                                           D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&b.resource));
    if (FAILED(hr)) {
      state_->FreeBufferBlocks(b);
      state_->buffers.erase(u.key);
      return fail("native-upload-buffer-create", hr);
    }
    b.state = D3D12_RESOURCE_STATE_COMMON;
    b.bytes = u.bytes;
    b.version = 0;
    b.last_use_fence = 0;
    ++state_->buffers_created;
  }
  profile::Count(profile::kUploadBytes, range_bytes);
  profile::Count(profile::kBufferUploadBytes, range_bytes);
  profile::Count(profile::kBufferUploads, 1);
  State::UploadPage* page = nullptr;
  size_t page_offset = 0;
  if (!state_->Upload(size_t(range_bytes), page, page_offset, hr)) return fail("native-upload-buffer-page", hr);
  auto& list = state_->batch_list;
  auto barrier = [&](D3D12_RESOURCE_STATES after) {
    if (b.state == after) return;
    D3D12_RESOURCE_BARRIER r{};
    r.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    r.Transition.pResource = b.resource.Get();
    r.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    r.Transition.StateBefore = b.state;
    r.Transition.StateAfter = after;
    list->ResourceBarrier(1, &r);
    b.state = after;
  };
  barrier(D3D12_RESOURCE_STATE_COPY_DEST);
  size_t at = 0;
  for (const auto& [offset, data] : u.ranges) {
    std::memcpy(page->cpu + page_offset + at, data.data(), data.size());
    list->CopyBufferRegion(b.resource.Get(), offset, page->buffer.Get(), page_offset + at, data.size());
    at += data.size();
  }
  barrier(kBufferRead);
  b.version = u.version;
  b.last_use_fence = state_->batch.fence;
  ++state_->buffer_uploads;
  const uint64_t batch_operations = ++state_->batch.operations;
  ++state_->operations;
  if (batch_operations >= kBatchOperationsMax && !state_->CloseBatch(batch_reason, hr)) return fail(batch_reason, hr);
  evidence = fmt::format("{{\"uploaded\":true,\"key\":{},\"created\":{},\"version\":{},\"bytes\":{},\"range_bytes\":{},"
                         "\"ranges\":{},\"buffers\":{}}}", u.key, create, u.version, u.bytes, range_bytes,
                         u.ranges.size(), state_->buffers.size());
  return true;
}

bool Backend::ReleaseBuffer(uint64_t key, std::string& evidence) {
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId()) {
    evidence = "{\"released\":false,\"reason\":\"native-buffer-release-owner\"}";
    return false;
  }
  auto it = state_->buffers.find(key);
  if (it == state_->buffers.end()) {
    evidence = fmt::format("{{\"released\":true,\"key\":{},\"buffer\":false}}", key);
    return true;
  }
  auto* fence = static_cast<plume::D3D12CommandFence*>(state_->fence.get());
  const uint64_t completed = fence->d3d->GetCompletedValue();
  // The open batch has not been submitted: its fence is above every completed value.
  const uint64_t last_use = it->second.last_use_fence;
  const bool in_flight = last_use && (completed == UINT64_MAX || completed < last_use);
  if (in_flight) {
    state_->retired_buffers.push_back(std::move(it->second));
    ++state_->buffers_retired;
  } else {
    it->second.resource.Reset();
    state_->FreeBufferBlocks(it->second);
    ++state_->buffers_released;
  }
  state_->buffers.erase(it);
  evidence = fmt::format("{{\"released\":true,\"key\":{},\"buffer\":true,\"deferred\":{},\"last_use_fence\":{},"
                         "\"observed_completed\":{}}}", key, in_flight, last_use, completed);
  return true;
}

bool Backend::ReadTexture(uint32_t key, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height,
                                       std::string& evidence) {
  rgba.clear();
  width = height = 0;
  auto fail = [&](const char* reason, HRESULT hr = E_FAIL) {
    evidence = fmt::format("{{\"read\":false,\"reason\":\"{}\",\"key\":{},\"hresult\":{}}}", reason, key,
                           static_cast<uint32_t>(hr));
    return false;
  };
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId() || state_->operation_poisoned ||
      state_->unproved_execution) return fail("native-read-owner-or-state");
  const char* batch_reason = nullptr;
  HRESULT batch_hr = S_OK;
  if (!state_->CloseBatch(batch_reason, batch_hr)) return fail(batch_reason, batch_hr);
  auto it = state_->textures.find(key);
  if (it == state_->textures.end() || !it->second.texture) return fail("native-read-texture-unknown");
  auto& texture = it->second;
  // A depth destination reads back its R32_FLOAT texels (4 bytes each, like RGBA8).
  if (!texture.content_defined) return fail("native-read-content-undefined");
  auto* device = static_cast<plume::D3D12Device*>(state_->device.get());
  auto* fence = static_cast<plume::D3D12CommandFence*>(state_->fence.get());
  auto* resource = static_cast<plume::D3D12Texture*>(texture.texture.get())->d3d;
  const auto desc = resource->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rows = 0;
  UINT64 row_bytes = 0, total = 0;
  device->d3d->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &row_bytes, &total);
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = total;
  buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  State::Submission submission;
  HRESULT hr = device->d3d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr, IID_PPV_ARGS(&submission.upload));
  if (FAILED(hr)) return fail("native-read-buffer", hr);
  hr = device->d3d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&submission.allocator));
  if (FAILED(hr)) return fail("native-read-allocator", hr);
  hr = device->d3d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, submission.allocator.Get(), nullptr,
                                      IID_PPV_ARGS(&submission.commands));
  if (FAILED(hr)) return fail("native-read-list", hr);
  if (texture.state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = texture.state;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    submission.commands->ResourceBarrier(1, &b);
    texture.state = D3D12_RESOURCE_STATE_COPY_SOURCE;
  }
  D3D12_TEXTURE_COPY_LOCATION to{}, from{};
  to.pResource = submission.upload.Get();
  to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  to.PlacedFootprint = footprint;
  from.pResource = resource;
  from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  submission.commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  hr = submission.commands->Close();
  if (FAILED(hr)) return fail("native-read-close", hr);
  submission.fence = fence->fenceValue;
  if (!submission.fence || submission.fence == UINT64_MAX) return fail("native-read-fence-exhausted");
  if (!state_->Submit(submission.commands.Get(), submission.fence, hr))
    return fail(hr == E_ABORT ? "native-read-refused-after-failure" : "native-read-signal", hr);
  state_->pending_signal = submission.fence;
  ++fence->fenceValue;
  texture.last_use_fence = submission.fence;
  const uint64_t read_fence = submission.fence;
  auto readback = submission.upload;
  state_->submissions.push_back(std::move(submission));
  ++state_->operations;
  std::string idle;
  if (!WaitIdle(idle)) return fail("native-read-wait");
  uint8_t* mapped = nullptr;
  const D3D12_RANGE range{0, SIZE_T(total)};
  hr = readback->Map(0, &range, reinterpret_cast<void**>(&mapped));
  if (FAILED(hr)) return fail("native-read-map", hr);
  width = UINT(desc.Width);
  height = desc.Height;
  rgba.resize(size_t(width) * height * 4);
  for (UINT y = 0; y < height; ++y)
    std::memcpy(&rgba[size_t(y) * width * 4], mapped + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch,
                size_t(width) * 4);
  const D3D12_RANGE none{0, 0};
  readback->Unmap(0, &none);
  evidence = fmt::format("{{\"read\":true,\"key\":{},\"size\":[{},{}],\"format\":{},\"fence\":{},\"operations\":{}}}",
                         key, width, height, static_cast<uint32_t>(desc.Format), read_fence, state_->operations);
  return true;
}

bool Backend::ReadSampled(uint32_t key, uint32_t level, std::vector<uint8_t>& rows_out,
                                       TextureFormat& format, uint32_t& width, uint32_t& height, uint32_t& row_bytes_out,
                                       std::string& evidence) {
  rows_out.clear();
  format = TextureFormat::kUnknown;
  width = height = row_bytes_out = 0;
  auto fail = [&](const char* reason, HRESULT hr = E_FAIL) {
    evidence = fmt::format("{{\"read\":false,\"reason\":\"{}\",\"key\":{},\"level\":{},\"hresult\":{}}}", reason,
                           key, level, static_cast<uint32_t>(hr));
    return false;
  };
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId() || state_->operation_poisoned ||
      state_->unproved_execution) return fail("native-read-owner-or-state");
  const char* batch_reason = nullptr;
  HRESULT batch_hr = S_OK;
  if (!state_->CloseBatch(batch_reason, batch_hr)) return fail(batch_reason, batch_hr);
  auto it = state_->sampled.find(key);
  if (it == state_->sampled.end() || !it->second.resource) return fail("native-read-sampled-unknown");
  auto& copy = it->second;
  if (level >= copy.levels) return fail("native-read-sampled-level");
  auto* device = static_cast<plume::D3D12Device*>(state_->device.get());
  auto* fence = static_cast<plume::D3D12CommandFence*>(state_->fence.get());
  ID3D12Resource* resource = copy.resource.Get();
  const auto desc = resource->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rows = 0;
  UINT64 row_bytes = 0, total = 0;
  device->d3d->GetCopyableFootprints(&desc, level, 1, 0, &footprint, &rows, &row_bytes, &total);
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = total;
  buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  State::Submission submission;
  HRESULT hr = device->d3d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr, IID_PPV_ARGS(&submission.upload));
  if (FAILED(hr)) return fail("native-read-buffer", hr);
  hr = device->d3d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&submission.allocator));
  if (FAILED(hr)) return fail("native-read-allocator", hr);
  hr = device->d3d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, submission.allocator.Get(), nullptr,
                                      IID_PPV_ARGS(&submission.commands));
  if (FAILED(hr)) return fail("native-read-list", hr);
  if (copy.state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = copy.state;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    submission.commands->ResourceBarrier(1, &b);
    copy.state = D3D12_RESOURCE_STATE_COPY_SOURCE;
  }
  D3D12_TEXTURE_COPY_LOCATION to{}, from{};
  to.pResource = submission.upload.Get();
  to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  to.PlacedFootprint = footprint;
  from.pResource = resource;
  from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  from.SubresourceIndex = level;
  submission.commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  hr = submission.commands->Close();
  if (FAILED(hr)) return fail("native-read-close", hr);
  submission.fence = fence->fenceValue;
  if (!submission.fence || submission.fence == UINT64_MAX) return fail("native-read-fence-exhausted");
  if (!state_->Submit(submission.commands.Get(), submission.fence, hr))
    return fail(hr == E_ABORT ? "native-read-refused-after-failure" : "native-read-signal", hr);
  state_->pending_signal = submission.fence;
  ++fence->fenceValue;
  copy.last_use_fence = submission.fence;
  auto readback = submission.upload;
  state_->submissions.push_back(std::move(submission));
  ++state_->operations;
  std::string idle;
  if (!WaitIdle(idle)) return fail("native-read-wait");
  uint8_t* mapped = nullptr;
  const D3D12_RANGE range{0, SIZE_T(total)};
  hr = readback->Map(0, &range, reinterpret_cast<void**>(&mapped));
  if (FAILED(hr)) return fail("native-read-map", hr);
  rows_out.resize(size_t(row_bytes) * rows);
  for (UINT y = 0; y < rows; ++y)
    std::memcpy(&rows_out[size_t(y) * row_bytes], mapped + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch,
                size_t(row_bytes));
  const D3D12_RANGE none{0, 0};
  readback->Unmap(0, &none);
  format = copy.format;
  width = std::max(UINT(desc.Width >> level), 1u);
  height = std::max(desc.Height >> level, 1u);
  row_bytes_out = uint32_t(row_bytes);
  evidence = fmt::format("{{\"read\":true,\"key\":{},\"level\":{},\"format\":{},\"size\":[{},{}],\"rows\":{},"
                         "\"row_bytes\":{}}}", key, level, uint32_t(format), width, height, rows, row_bytes);
  return true;
}

bool Backend::ReadSurface(uint32_t key, uint64_t after_fence, std::vector<uint8_t>& rgba,
                                       uint32_t& width, uint32_t& height, std::string& evidence) {
  rgba.clear();
  width = height = 0;
  auto fail = [&](const char* reason, HRESULT hr = E_FAIL) {
    evidence = fmt::format("{{\"read\":false,\"reason\":\"{}\",\"key\":{},\"after_fence\":{},\"hresult\":{}}}", reason,
                           key, after_fence, static_cast<uint32_t>(hr));
    return false;
  };
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId() || state_->operation_poisoned ||
      state_->unproved_execution) return fail("native-read-surface-owner-or-state");
  const char* batch_reason = nullptr;
  HRESULT batch_hr = S_OK;
  if (!state_->CloseBatch(batch_reason, batch_hr)) return fail(batch_reason, batch_hr);
  auto it = state_->surfaces.find(key);
  if (it == state_->surfaces.end() || !it->second.texture) return fail("native-read-surface-unknown");
  auto& surface = it->second;
  if (surface.desc.format != SurfaceFormat::Color8888 || surface.desc.samples != 1)
    return fail("native-read-surface-format");
  auto* device = static_cast<plume::D3D12Device*>(state_->device.get());
  auto* fence = static_cast<plume::D3D12CommandFence*>(state_->fence.get());
  // The read follows the operation of after_fence, and nothing else has used
  // the surface since.
  if (!after_fence || after_fence >= fence->fenceValue || surface.last_use_fence != after_fence)
    return fail("native-read-surface-fence");
  HRESULT hr = S_OK;
  if (fence->d3d->GetCompletedValue() < after_fence) {
    if (FAILED(hr = fence->d3d->SetEventOnCompletion(after_fence, fence->fenceEvent)))
      return fail("native-read-surface-event", hr);
    if (WaitForSingleObject(fence->fenceEvent, 5000) != WAIT_OBJECT_0) return fail("native-read-surface-wait");
  }
  const uint64_t completed_before = fence->d3d->GetCompletedValue();
  if (completed_before == UINT64_MAX || completed_before < after_fence) return fail("native-read-surface-incomplete");
  auto* resource = static_cast<plume::D3D12Texture*>(surface.texture.get())->d3d;
  const auto desc = resource->GetDesc();
  if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) return fail("native-read-surface-host-format");
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rows = 0;
  UINT64 row_bytes = 0, total = 0;
  device->d3d->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &row_bytes, &total);
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = total;
  buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  State::Submission submission;
  hr = device->d3d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                            nullptr, IID_PPV_ARGS(&submission.upload));
  if (FAILED(hr)) return fail("native-read-surface-buffer", hr);
  hr = device->d3d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&submission.allocator));
  if (FAILED(hr)) return fail("native-read-surface-allocator", hr);
  hr = device->d3d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, submission.allocator.Get(), nullptr,
                                      IID_PPV_ARGS(&submission.commands));
  if (FAILED(hr)) return fail("native-read-surface-list", hr);
  const auto state_before = surface.state;
  if (surface.state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = surface.state;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    submission.commands->ResourceBarrier(1, &b);
    surface.state = D3D12_RESOURCE_STATE_COPY_SOURCE;
  }
  D3D12_TEXTURE_COPY_LOCATION to{}, from{};
  to.pResource = submission.upload.Get();
  to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  to.PlacedFootprint = footprint;
  from.pResource = resource;
  from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  submission.commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  hr = submission.commands->Close();
  if (FAILED(hr)) return fail("native-read-surface-close", hr);
  submission.fence = fence->fenceValue;
  if (!submission.fence || submission.fence == UINT64_MAX) return fail("native-read-surface-fence-exhausted");
  if (!state_->Submit(submission.commands.Get(), submission.fence, hr))
    return fail(hr == E_ABORT ? "native-read-surface-refused-after-failure" : "native-read-surface-signal", hr);
  state_->pending_signal = submission.fence;
  ++fence->fenceValue;
  surface.last_use_fence = submission.fence;
  const uint64_t read_fence = submission.fence;
  auto readback = submission.upload;
  state_->submissions.push_back(std::move(submission));
  ++state_->operations;
  std::string idle;
  if (!WaitIdle(idle)) return fail("native-read-surface-wait-idle");
  uint8_t* mapped = nullptr;
  const D3D12_RANGE range{0, SIZE_T(total)};
  hr = readback->Map(0, &range, reinterpret_cast<void**>(&mapped));
  if (FAILED(hr)) return fail("native-read-surface-map", hr);
  width = UINT(desc.Width);
  height = desc.Height;
  rgba.resize(size_t(width) * height * 4);
  for (UINT y = 0; y < height; ++y)
    std::memcpy(&rgba[size_t(y) * width * 4], mapped + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch,
                size_t(width) * 4);
  const D3D12_RANGE none{0, 0};
  readback->Unmap(0, &none);
  evidence = fmt::format("{{\"read\":true,\"key\":{},\"size\":[{},{}],\"format\":{},\"samples\":{},\"state_before\":{},"
                         "\"after_fence\":{},\"completed_before_copy\":{},\"read_fence\":{},\"row_pitch\":{},"
                         "\"operations\":{}}}", key, width, height, static_cast<uint32_t>(desc.Format),
                         surface.desc.samples, static_cast<uint32_t>(state_before), after_fence, completed_before,
                         read_fence, footprint.Footprint.RowPitch, state_->operations);
  return true;
}
#if defined(NATIVE_RENDERER_ENGINE_FAULT_TEST)
bool Backend::SeedSampledForTest(uint32_t address, bool in_flight, std::string& evidence) {
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId() || state_->sampled.count(address)) return false;
  HRESULT hr = S_OK;
  const char* reason = nullptr;
  if (in_flight && !state_->OpenBatch(reason, hr)) return false;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = desc.Height = 4;
  desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  auto& s = state_->sampled[address];
  hr = static_cast<plume::D3D12Device*>(state_->device.get())->d3d->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s.resource));
  if (FAILED(hr)) { state_->sampled.erase(address); return false; }
  s.format = TextureFormat::kR8G8B8A8Unorm;
  s.width = s.height = 4;
  s.levels = 1;
  if (!state_->free_views.empty()) { s.view = state_->free_views.back(); state_->free_views.pop_back(); }
  else s.view = state_->next_view++;
  s.last_use_fence = in_flight ? state_->batch.fence : 0;
  evidence = fmt::format("{{\"seeded\":true,\"address\":{},\"view\":{},\"last_use_fence\":{},\"counts\":{}}}", address,
                         s.view, s.last_use_fence, ResourceCounts());
  return true;
}

bool Backend::ReadBufferForTest(uint64_t key, std::vector<uint8_t>& bytes, std::string& evidence) {
  bytes.clear();
  auto fail = [&](const char* reason, HRESULT hr = E_FAIL) {
    evidence = fmt::format("{{\"read\":false,\"reason\":\"{}\",\"key\":{},\"hresult\":{}}}", reason, key,
                           static_cast<uint32_t>(hr));
    return false;
  };
  if (!state_->ready || state_->owner_thread != GetCurrentThreadId()) return fail("native-read-owner-or-state");
  const char* batch_reason = nullptr;
  HRESULT hr = S_OK;
  if (!state_->CloseBatch(batch_reason, hr)) return fail(batch_reason, hr);
  auto it = state_->buffers.find(key);
  if (it == state_->buffers.end()) return fail("native-read-buffer-unknown");
  auto& b = it->second;
  auto* device = static_cast<plume::D3D12Device*>(state_->device.get());
  auto* fence = static_cast<plume::D3D12CommandFence*>(state_->fence.get());
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = b.bytes;
  buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  State::Submission submission;
  if (FAILED(hr = device->d3d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&submission.upload))) ||
      FAILED(hr = device->d3d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      IID_PPV_ARGS(&submission.allocator))) ||
      FAILED(hr = device->d3d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, submission.allocator.Get(),
                                                 nullptr, IID_PPV_ARGS(&submission.commands))))
    return fail("native-read-buffer-objects", hr);
  if (b.state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = b.resource.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = b.state;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    submission.commands->ResourceBarrier(1, &barrier);
    b.state = D3D12_RESOURCE_STATE_COPY_SOURCE;
  }
  submission.commands->CopyBufferRegion(submission.upload.Get(), 0, b.resource.Get(), 0, b.bytes);
  if (FAILED(hr = submission.commands->Close())) return fail("native-read-close", hr);
  submission.fence = fence->fenceValue;
  if (!state_->Submit(submission.commands.Get(), submission.fence, hr)) return fail("native-read-signal", hr);
  state_->pending_signal = submission.fence;
  ++fence->fenceValue;
  b.last_use_fence = submission.fence;
  auto readback = submission.upload;
  state_->submissions.push_back(std::move(submission));
  std::string idle;
  if (!WaitIdle(idle)) return fail("native-read-wait");
  uint8_t* mapped = nullptr;
  const D3D12_RANGE range{0, SIZE_T(b.bytes)};
  if (FAILED(hr = readback->Map(0, &range, reinterpret_cast<void**>(&mapped)))) return fail("native-read-map", hr);
  bytes.assign(mapped, mapped + b.bytes);
  const D3D12_RANGE none{0, 0};
  readback->Unmap(0, &none);
  evidence = fmt::format("{{\"read\":true,\"key\":{},\"bytes\":{}}}", key, b.bytes);
  return true;
}
#endif
}  // namespace gpu::d3d12

namespace gpu {
std::unique_ptr<Backend> CreateD3d12Backend(std::shared_ptr<FailureChannel> failures) {
  return std::make_unique<d3d12::Backend>(std::move(failures));
}
}  // namespace gpu
