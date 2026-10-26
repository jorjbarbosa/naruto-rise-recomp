// The Vulkan backend of the engine (backend.h). Plume creates the device, the
// queue, the surfaces with their framebuffers and the command lists, and
// records the clears; the barriers, the copies, the sampled copies, the
// buffers and the submissions are Vulkan's own, and the completion of each
// submission is the service of completion.h.
//
// Each batch, readback or readiness submission has a sequence, one above the
// previous; it is the "fence" of the operations (engine.h), and a resource
// recorded by work of a sequence is released once that sequence completed. A
// command list goes to the queue only through FailureChannel::Authorize.
//
// Draws and resolves are in draw.inc/resolve.inc; presentation and its WSI
// retirement are in present.inc/output.inc, and the UI layer of the Present in
// ui.inc. They share this batch lifetime.
#include "gpu/backend/backend.h"
#include "gpu/backend/reconfigure.h"
#include "gpu/backend/lifecycle.h"
#include "gpu/backend/pipeline_set.h"
#include "gpu/backend/regions.h"
#include "gpu/backend/vulkan/completion.h"
#include "gpu/backend/vulkan/formats.h"
#include "gpu/backend/vulkan/shader_contract.h"
#include "gpu/backend/vulkan/graphics.h"
#include "gpu/backend/vulkan/draw_cache.h"
#include "gpu/backend/output_plan.h"
#include "gpu/common/title_loading.h"
#include "gpu/d3d/init/initializer_services.h"
#include "gpu/diagnostics/profile.h"
#include "gpu/shaders/package.h"
#if defined(__linux__)
// Wayland's WSI: not in Plume's headers, which build for Xlib on Linux.
#include <vulkan/vulkan_wayland.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <unordered_map>
#include <fmt/format.h>
#include <fmt/ranges.h>

namespace plume { std::unique_ptr<RenderInterface> CreateVulkanInterface(); }

namespace gpu::vulkan {
namespace {
// The bound of the owner's waits (WaitIdle, the readiness and the readbacks).
constexpr std::chrono::milliseconds kWaitBudget{5000};
constexpr size_t kUploadPageBytes = size_t(8) << 20;  // Pooled page; larger requests get their own.
constexpr VkDeviceSize kUploadAlign = 512;             // Texel blocks and the texture rows of any format.
constexpr uint64_t kBatchOperationsMax = 2048;         // A longer batch closes without waiting.

// How the next command uses an image or a buffer: the image's layout (none
// for a buffer), the stages and the accesses.
struct Use {
  VkImageLayout layout;
  VkPipelineStageFlags stages;
  VkAccessFlags access;
};
constexpr Use kCopySource{VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_ACCESS_TRANSFER_READ_BIT};
constexpr Use kCopyDest{VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT};
constexpr Use kColorTarget{VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
constexpr Use kDepthTarget{VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
constexpr Use kShaderRead{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_SHADER_READ_BIT};
constexpr Use kBufferCopySource{VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_ACCESS_TRANSFER_READ_BIT};
constexpr Use kBufferCopyDest{VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_ACCESS_TRANSFER_WRITE_BIT};
constexpr Use kBufferDraw{VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                          VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT};
constexpr VkAccessFlags kWrites = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                  VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

// The synchronization state of an image or a buffer, apart from its content
// (defined or not) and from the sequence of its last use: the image's layout;
// the stages after which its last write (or layout transition) is complete
// and the write still to make available; the stages and accesses that write
// was made visible to; and the stages that read it since.
struct SyncState {
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkPipelineStageFlags source_stages = 0, visible_stages = 0, read_stages = 0;
  VkAccessFlags source_access = 0, visible_access = 0;
};

// Advances state to the use. True with the source of the barrier the use
// needs: a write or a layout change waits for the last write and for the reads
// since; a read waits for the last write unless it is visible to the read's
// stages and accesses already.
bool Synchronize(SyncState& state, const Use& use, bool layout_change, VkPipelineStageFlags& src_stages,
                 VkAccessFlags& src_access) {
  const VkAccessFlags writes = use.access & kWrites;
  if (!writes && !layout_change) {
    state.read_stages |= use.stages;
    if (!state.source_stages ||
        (!(use.stages & ~state.visible_stages) && !(use.access & ~state.visible_access))) return false;
    src_stages = state.source_stages;
    src_access = state.source_access;
    state.visible_stages |= use.stages;
    state.visible_access |= use.access;
    return true;
  }
  src_stages = state.source_stages | state.read_stages;
  if (!src_stages) src_stages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
  src_access = state.source_access;
  // The barrier makes the earlier writes available: later ones chain from its stages.
  state.layout = use.layout;
  state.source_stages = use.stages;
  state.source_access = writes;
  state.visible_stages = writes ? 0 : use.stages;
  state.visible_access = writes ? 0 : use.access;
  state.read_stages = writes ? 0 : use.stages;
  return true;
}

// Records the barrier an image needs before the use, if any. discard: the
// content is not kept (every texel will be written).
void Transition(VkCommandBuffer commands, VkImage image, VkImageAspectFlags aspect, SyncState& state, const Use& use,
                bool discard = false) {
  const VkImageLayout old_layout = discard ? VK_IMAGE_LAYOUT_UNDEFINED : state.layout;
  VkPipelineStageFlags src_stages = 0;
  VkAccessFlags src_access = 0;
  if (!Synchronize(state, use, discard || state.layout != use.layout, src_stages, src_access)) return;
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = src_access;
  barrier.dstAccessMask = use.access;
  barrier.oldLayout = old_layout;
  barrier.newLayout = use.layout;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
  vkCmdPipelineBarrier(commands, src_stages, use.stages, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// Records the barrier a buffer needs before the use, if any.
void Transition(VkCommandBuffer commands, VkBuffer buffer, SyncState& state, const Use& use) {
  VkPipelineStageFlags src_stages = 0;
  VkAccessFlags src_access = 0;
  if (!Synchronize(state, use, false, src_stages, src_access)) return;
  VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  barrier.srcAccessMask = src_access;
  barrier.dstAccessMask = use.access;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = buffer;
  barrier.size = VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(commands, src_stages, use.stages, 0, 0, nullptr, 1, &barrier, 0, nullptr);
}

// A buffer of the Vulkan Memory Allocator, destroyed with its allocation.
// Mapped for its life when created for the host (upload pages, readbacks).
struct VmaBuffer {
  VmaAllocator allocator = VK_NULL_HANDLE;
  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
  uint8_t* mapped = nullptr;
  VkDeviceSize size = 0;
  VmaBuffer() = default;
  VmaBuffer(const VmaBuffer&) = delete;
  VmaBuffer& operator=(const VmaBuffer&) = delete;
  ~VmaBuffer() {
    if (buffer) vmaDestroyBuffer(allocator, buffer, allocation);
  }
};
// host: VMA_ALLOCATION_CREATE_HOST_ACCESS_* for a mapped buffer, 0 for device memory.
VkResult CreateBuffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage,
                      VmaAllocationCreateFlags host, std::shared_ptr<VmaBuffer>& out) {
  auto made = std::make_shared<VmaBuffer>();
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VmaAllocationCreateInfo allocation{};
  allocation.usage = VMA_MEMORY_USAGE_AUTO;
  allocation.flags = host ? host | VMA_ALLOCATION_CREATE_MAPPED_BIT : 0;
  VmaAllocationInfo allocated{};
  const VkResult result = vmaCreateBuffer(allocator, &info, &allocation, &made->buffer, &made->allocation, &allocated);
  if (result != VK_SUCCESS) return result;
  made->allocator = allocator;
  made->mapped = static_cast<uint8_t*>(allocated.pMappedData);
  made->size = size;
  out = std::move(made);
  return VK_SUCCESS;
}

// An image of the Vulkan Memory Allocator, destroyed with its allocation: a
// sampled copy (2D, or a cube of six layers), with every level.
struct VmaImage {
  VmaAllocator allocator = VK_NULL_HANDLE;
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
  VmaImage() = default;
  VmaImage(const VmaImage&) = delete;
  VmaImage& operator=(const VmaImage&) = delete;
  ~VmaImage() {
    if (image) vmaDestroyImage(allocator, image, allocation);
  }
};
VkResult CreateImage(VmaAllocator allocator, VkFormat format, uint32_t width, uint32_t height, uint32_t levels,
                     bool cube, std::shared_ptr<VmaImage>& out, VkImageUsageFlags extra_usage = 0,
                     VkImageType type = VK_IMAGE_TYPE_2D) {
  auto made = std::make_shared<VmaImage>();
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
  info.imageType = type;
  info.format = format;
  info.extent = {width, height, 1};
  info.mipLevels = levels;
  info.arrayLayers = cube ? 6 : 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | extra_usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VmaAllocationCreateInfo allocation{};
  allocation.usage = VMA_MEMORY_USAGE_AUTO;
  const VkResult result = vmaCreateImage(allocator, &info, &allocation, &made->image, &made->allocation, nullptr);
  if (result != VK_SUCCESS) return result;
  made->allocator = allocator;
  out = std::move(made);
  return VK_SUCCESS;
}

const char* LayoutName(VkImageLayout layout) {
  switch (layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED: return "undefined";
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return "copy-source";
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return "copy-dest";
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return "color-target";
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return "depth-target";
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return "shader-read";
    default: return "other";
  }
}

std::string Version(uint32_t version) {
  return fmt::format("{}.{}.{}", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
                     VK_API_VERSION_PATCH(version));
}

// A name from the driver or the user, as a JSON string body.
std::string JsonText(std::string_view text) {
  std::string out;
  for (char c : text.substr(0, 256)) {
    if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126) { out += '?'; continue; }
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

}  // namespace

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
  bool AttachWindow(const platform::NativeWindow& window, std::string& evidence, const PresentMode&) override;
  bool DetachWindow(std::string& evidence) override;
  bool SetGammaRamp(const std::array<uint16_t, 768>&, std::string& evidence) override;
  bool SetVsync(bool, std::string& evidence) override;
  bool Reconfigure(const RenderMode&, const PresentMode&, std::string& evidence) override;
  bool StageFront(uint32_t, uint32_t& slot, std::string& evidence) override;
  bool Present(uint32_t, bool, std::vector<uint8_t>& bgra, uint32_t& width, uint32_t& height,
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
  bool DrawIndexed(const DrawRequest&, uint64_t& fence, std::string& evidence) override;
  std::string PipelineList() override;
  std::vector<PipelineDesc> PipelineDescs() override;
  bool PreparePipelines(std::string& evidence) override;
  bool PrecompilePipeline(const PipelineDesc&, bool& present, std::string& evidence) override;
  void PrefetchPipelines(const std::vector<DrawRequest>&) override;
  bool PostPipeline(const PipelineDesc&) override;
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
  CompletionTestHooks* CompletionHooksForTest() override;
#endif

 private:
  // The view slot of a sampled texture from the free ones, or a new one.
  uint32_t AllocateView();
  // WaitIdle (close: the open batch is submitted first) and WaitSubmitted.
  bool Wait(bool close, std::string& evidence);
  struct State;
  std::unique_ptr<State> state_;
};

struct Backend::State {
  gpu::InitializerServices frontend;
  std::unique_ptr<plume::RenderInterface> render;
  std::unique_ptr<plume::RenderDevice> device;
  std::unique_ptr<plume::RenderCommandQueue> queue;
  plume::VulkanDevice* vulkan = nullptr;  // The device's Vulkan side.
  bool timeline_semaphore = false;  // Enabled by the patched Plume when supported.
  ShaderContract contract;          // The SPIR-V contract of the build, checked against the device.
  // The session's failure channel: every submission goes through it.
  std::shared_ptr<FailureChannel> failures;
  std::unique_ptr<Completion> completion;
  uint64_t next_sequence = 1;    // The sequence of the next submission.
  uint64_t pending_sequence = 0;  // The last submitted, until an owner wait sees it complete.
  bool ready = false;
  bool operation_poisoned = false;
  bool unproved_execution = false;  // A submission's outcome is unknown.
  bool device_lost = false;         // A Vulkan call answered VK_ERROR_DEVICE_LOST.
  uint32_t owner_thread = 0;
  uint64_t epoch = 0;
  RenderMode render_mode;
  std::atomic<uint32_t> pipeline_msaa{0};
  uint32_t PipelineSamples(uint32_t guest) const {
    const uint32_t count = pipeline_msaa.load(); return guest > 1 && count ? count : guest;
  }
  void Observe(VkResult result) { device_lost = device_lost || result == VK_ERROR_DEVICE_LOST; }

  // Submits a recorded command buffer (none: only the fence, the readiness) as
  // sequence next_sequence, unless the session holds a failure (refused). A
  // failed submission poisons the owner.
  bool Submit(VkCommandBuffer commands, VkResult& result, bool& refused);

  // Draws, clears, uploads and resolves are recorded into one open batch: a
  // pooled command list and persistent upload pages. A batch closes with one
  // submission; until its sequence completes it keeps its command list, its
  // upload pages and the resources replaced while it could still read them.
  struct UploadPage {
    std::shared_ptr<VmaBuffer> buffer;
    VkDeviceSize used = 0;
    VkDeviceAddress address = 0;
  };
  DrawCacheCounters draw_counters;
  struct Batch {
    uint64_t sequence = 0;  // Its submission's, known while it records.
    std::unique_ptr<plume::RenderCommandList> list;
    std::vector<UploadPage> pages;
    std::vector<std::shared_ptr<void>> retained;
    std::vector<uint32_t> views;  // View slots back to free_views once it completed.
    uint64_t operations = 0;
    std::unique_ptr<DrawCache> graphics;
    uint64_t vs_version = 0, ps_version = 0;
    VkDeviceAddress vs_constants = 0, ps_constants = 0;
    bool draw_pass_active = false;
    VkImage draw_color{}, draw_depth{};
    bool draw_bindings_valid = false;
    VkPipeline draw_pipeline{};
    std::array<VkDescriptorSet, 4> draw_sets{};
    VkViewport draw_viewport{};
    VkRect2D draw_scissor{};
    std::array<float, 4> draw_blend{};
    uint32_t draw_stencil = 0;
    Topology draw_topology = Topology::kTriangleList;
    std::array<VkBuffer, 8> draw_streams{};
    std::array<VkDeviceSize, 8> draw_offsets{}, draw_strides{};
    uint32_t draw_stream_count = 0;
    VkBuffer draw_indices{};
    VkDeviceSize draw_index_offset{};
    bool draw_index32 = false;
  };
  bool batch_open = false, batch_refused = false;
  Batch batch;
  std::deque<Batch> batches_in_flight;
  std::vector<std::unique_ptr<DrawCache>> free_draw_caches;
  std::vector<std::unique_ptr<plume::RenderCommandList>> free_lists;
  std::vector<UploadPage> free_pages;
  uint64_t batches_closed = 0, lists_created = 0, upload_pages_created = 0;
  VkCommandBuffer DrawCommands() const { return static_cast<plume::VulkanCommandList*>(batch.list.get())->vk; }
  void EndDrawPass() {
    if (!batch.draw_pass_active) return;
    vkCmdEndRenderPass(DrawCommands());
    batch.draw_pass_active = false;
    batch.draw_bindings_valid = false;
  }
  // Transfers, clears, auxiliary passes and submission leave the draw pass.
  VkCommandBuffer Commands() { EndDrawPass(); return DrawCommands(); }
  void DrawTransition(VkImage image, VkImageAspectFlags aspect, SyncState& state, const Use& use) {
    SyncState next = state;
    VkPipelineStageFlags stages{}; VkAccessFlags access{};
    if (Synchronize(next, use, state.layout != use.layout, stages, access)) EndDrawPass();
    Transition(DrawCommands(), image, aspect, state, use);
  }
  void DrawTransition(VkBuffer buffer, SyncState& state, const Use& use) {
    SyncState next = state;
    VkPipelineStageFlags stages{}; VkAccessFlags access{};
    if (Synchronize(next, use, false, stages, access)) EndDrawPass();
    Transition(DrawCommands(), buffer, state, use);
  }
  // A command list from the pool, begun; null with the result on failure.
  std::unique_ptr<plume::RenderCommandList> BeginList(VkResult& result);
  // Ends the list's render pass and the list.
  VkResult EndList(plume::RenderCommandList& list);
  bool OpenBatch(const char*& reason, VkResult& result);
  // Submits the open batch. After a failure it is neither closed nor submitted.
  bool CloseBatch(const char*& reason, VkResult& result);
  // Upload memory of the open batch: pooled mapped pages, kUploadAlign aligned.
  bool Upload(VkDeviceSize bytes, UploadPage*& page, VkDeviceSize& offset, VkResult& result);
  // The readbacks: one command list and buffer each, kept until they complete.
  struct Readback {
    uint64_t sequence = 0;
    std::unique_ptr<plume::RenderCommandList> list;
    std::shared_ptr<VmaBuffer> buffer;
  };
  std::vector<Readback> readbacks;
  // A new readback buffer of bytes: record copies into it (its barrier
  // first) in a command list of its own, then the barrier to the host; the
  // list is submitted and waited for. False with the reason and the result.
  bool ReadBack(VkDeviceSize bytes, const std::function<void(VkCommandBuffer, VkBuffer)>& record,
                uint64_t& sequence, std::shared_ptr<VmaBuffer>& buffer, const char*& reason, VkResult& result);
  // The readback of image regions (in the readback buffer's layout).
  bool ReadImage(VkImage image, SyncState& state, VkDeviceSize bytes, const VkBufferImageCopy& region,
                 uint64_t& sequence, std::shared_ptr<VmaBuffer>& buffer, const char*& reason, VkResult& result) {
    return ReadBack(bytes, [&](VkCommandBuffer commands, VkBuffer target) {
      Transition(commands, image, VK_IMAGE_ASPECT_COLOR_BIT, state, kCopySource);
      vkCmdCopyImageToBuffer(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, 1, &region);
    }, sequence, buffer, reason, result);
  }
  // Returns what the work up to completed held: command lists, upload pages,
  // view slots and the resources released or replaced while in flight.
  void Reclaim(uint64_t completed);
  bool InFlight(uint64_t last_use) const { return last_use > completion->Completed(); }

  struct Surface {
    SurfaceDesc desc;
    SurfaceDesc guest;
    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderFramebuffer> framebuffer;  // The surface alone: its clears.
    SyncState state;
    uint64_t last_use = 0;  // Highest sequence of submitted or recorded work using it.
    VkImage Image() const { return static_cast<plume::VulkanTexture*>(texture.get())->vk; }
    VkImageAspectFlags Aspect() const {
      return desc.format == SurfaceFormat::DepthStencil ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                                                        : VK_IMAGE_ASPECT_COLOR_BIT;
    }
  };
  std::unordered_map<uint32_t, Surface> surfaces;
  std::vector<Surface> retired_surfaces;  // Released, waiting for their sequence.
  // Own textures of guest texture headers: the resolve destinations.
  struct Texture {
    std::unique_ptr<plume::RenderTexture> texture;
    uint32_t width = 0, height = 0;
    bool depth = false, content_defined = false;
    bool render_target = false;  // Created for the resolve passes (depth, one color sample).
    SyncState state;
    uint64_t last_use = 0;
    uint32_t view = 0;  // Descriptor slot once sampled; 0 = none.
    VkImage Image() const { return static_cast<plume::VulkanTexture*>(texture.get())->vk; }
  };
  std::unordered_map<uint32_t, Texture> textures;
  std::vector<Texture> retired_textures;
  uint64_t textures_released = 0, textures_retired = 0;
  // Sampled copies of guest texture memory, keyed by the caller.
  struct Sampled {
    std::shared_ptr<VmaImage> image;
    TextureFormat format = TextureFormat::kUnknown;
    uint32_t width = 0, height = 0, levels = 0, view = 0;
    uint64_t layout = 0, version = 0;
    bool cube = false;  // Six array layers.
    SyncState state;
    uint64_t last_use = 0;
  };
  std::unordered_map<uint32_t, Sampled> sampled;
  std::vector<Sampled> retired_sampled;
  uint64_t sampled_released = 0, sampled_retired = 0;
  // Vertex and index buffers of the draw layer (UploadBuffer), keyed by the
  // caller; released at once or retired with the sequence of their last use.
  struct Buffer {
    std::shared_ptr<VmaBuffer> buffer;
    uint32_t bytes = 0;
    uint64_t version = 0, last_use = 0;
    SyncState state;
  };
  std::unordered_map<uint64_t, Buffer> buffers;
  std::vector<Buffer> retired_buffers;
  uint64_t buffers_created = 0, buffers_released = 0, buffers_retired = 0, buffer_uploads = 0;
  // Resource view identities used by the lifetime counters and host tests: a slot is
  // reused only once no submitted work can still read it.
  uint32_t next_view = 1;
  std::vector<uint32_t> free_views;
  uint64_t operations = 0;
  PipelineSet pipeline_set;  // Shared scheduling policy; configured by EnsureGraphics.
  VkPipelineLayout draw_layout = VK_NULL_HANDLE, resolve_layout = VK_NULL_HANDLE;
  std::array<VkDescriptorSetLayout, 4> draw_sets{};
  VkDescriptorSetLayout resolve_set = VK_NULL_HANDLE;
  std::map<std::string, uint32_t> vertex_locations;
  bool custom_border = false;
  std::array<std::shared_ptr<VmaImage>, 3> fallback_images;
  std::array<VkImageView, 3> fallback_views{};
  bool fallback_initialized = false;
  std::unordered_map<uint64_t, PipelineSet::Pipeline> pipelines;
  std::map<uint64_t, PipelineDesc> pipeline_descs;
  std::map<uint64_t, std::string> pipeline_origins;
  std::map<uint64_t, uint32_t> pipeline_worlds, pipeline_create_us;
  uint64_t depth_resolves = 0;
  platform::NativeWindow window;  // Kind kNone while no window is attached.
  bool Attached() const { return window.kind != platform::NativeWindow::Kind::kNone; }
  VkSurfaceKHR window_surface{};
  VkSwapchainKHR swapchain{};
  VkExtent2D swap_extent{};
  VkPresentModeKHR swap_mode = VK_PRESENT_MODE_FIFO_KHR;
  bool recreate_swapchain = false;
  uint64_t swap_generation = 0, staged_frames = 0, presents = 0;
  PresentMode present_mode;
  VkSemaphore submit_signal{};
  VkFence acquire_fence{};
  bool acquire_pending = false;
  struct BackImage {
    VkImage image{};
    SyncState state;
    VkSemaphore rendered{};
    VkFence presented{};
    bool pending = false;
  };
  std::vector<BackImage> back_images;
  struct OutputImage {
    std::shared_ptr<VmaImage> image;
    SyncState state;
    uint32_t width = 0, height = 0;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    uint64_t last_use = 0;
    bool staged = false, presented = true;
  };
  std::array<OutputImage, 2> stages;
  std::array<OutputImage, 8> effects;
  std::array<uint16_t, 768> gamma{};
  std::array<VkDescriptorSetLayout, 3> output_sets{};
  std::array<VkPipelineLayout, 3> output_layouts{};
  std::map<std::pair<std::string, VkFormat>, PipelineSet::Pipeline> output_pipelines;
  std::map<std::string, VkPipeline> compute_pipelines;
  bool WaitPresent(VkFence fence, std::string& evidence);
  bool RetireSwapchain(std::string& evidence);
  bool CreateSwapchain(uint32_t width, uint32_t height, std::string& evidence);
  bool EnsureOutput(std::string& evidence);
  bool OutputTarget(OutputImage& target, uint32_t width, uint32_t height, VkFormat format, bool storage, std::string& evidence);
  bool OutputPass(OutputImage& input, VkImage output, SyncState& output_state, VkFormat format,
                  uint32_t width, uint32_t height, const char* shader, unsigned layout,
                  const std::array<uint32_t, 12>& constants, VkBuffer ramp, VkDeviceSize ramp_offset, std::string& evidence);
  bool OutputEffects(OutputImage& source, BackImage& back, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                     VkBuffer ramp, VkDeviceSize ramp_offset, std::string& evidence);
  void ReleaseOutput();
  // The UI layer of the Present (ui.inc): RGBA8 textures by key, each with its
  // view and descriptor set (key 0 the white texture), the frame of SetUiFrame
  // and the pipeline of ui_pass.hlsl. A replaced or released texture waits for
  // the sequence of its last use.
  struct UiTexture {
    std::shared_ptr<VmaImage> image;
    std::shared_ptr<GraphicsResources> objects;  // Its view and the pool of its set.
    VkDescriptorSet set{};
    uint32_t width = 0, height = 0;
    SyncState state;
    uint64_t last_use = 0;
  };
  std::unordered_map<uint32_t, UiTexture> ui_textures;
  std::vector<UiTexture> retired_ui_textures;
  UiFrame ui_frame;
  VkSampler ui_sampler{};
  VkDescriptorSetLayout ui_set{};
  VkPipelineLayout ui_layout{};
  PipelineSet::Pipeline ui_pipeline;
  // Draws whose texture was released after their frame was set are skipped.
  uint64_t ui_texture_uploads = 0, ui_frames_drawn = 0, ui_draws_skipped = 0;
  bool UiFrameVisible() const {
    return ui_frame.width > 0 && ui_frame.height > 0 && !ui_frame.draws.empty() && !ui_frame.indices.empty();
  }
  bool EnsureUiLayer(std::string& evidence);
  bool WriteUiTexture(uint32_t key, uint32_t width, uint32_t height, const uint8_t* rgba, std::string& evidence);
  bool DrawUi(BackImage& back, std::string& evidence);
  void ReleaseUi();
  std::map<std::string, PipelineSet::Pipeline> resolve_pipelines;
  std::array<uint8_t, 4096> vs_block{}, ps_block{};
  bool EnsureGraphics(std::string& evidence);
  bool ResolvePass(Surface& source, Texture& dest, int32_t sample, std::string& evidence);
  void ReleaseGraphics() {
    for (auto view : fallback_views) if (view) vkDestroyImageView(vulkan->vk, view, nullptr);
    fallback_views = {}; fallback_images = {}; fallback_initialized = false;
    pipelines.clear(); resolve_pipelines.clear(); pipeline_descs.clear();
    pipeline_origins.clear(); pipeline_worlds.clear(); pipeline_create_us.clear();
    if (draw_layout) vkDestroyPipelineLayout(vulkan->vk, draw_layout, nullptr);
    if (resolve_layout) vkDestroyPipelineLayout(vulkan->vk, resolve_layout, nullptr);
    for (auto layout : draw_sets) if (layout) vkDestroyDescriptorSetLayout(vulkan->vk, layout, nullptr);
    if (resolve_set) vkDestroyDescriptorSetLayout(vulkan->vk, resolve_set, nullptr);
    draw_layout = resolve_layout = VK_NULL_HANDLE; draw_sets = {}; resolve_set = VK_NULL_HANDLE;
  }
#if defined(NATIVE_RENDERER_ENGINE_FAULT_TEST)
  // The hold of the host tests: while hold_value is above the value signaled,
  // each submission waits for it on the timeline semaphore hold.
  class Hooks;
  std::unique_ptr<Hooks> hooks;
  VkSemaphore hold = VK_NULL_HANDLE;
  std::atomic<uint64_t> hold_value{0}, hold_signaled{0};
#endif
};

#include "gpu/backend/vulkan/draw.inc"
#include "gpu/backend/vulkan/resolve.inc"
#include "gpu/backend/vulkan/present.inc"
#include "gpu/backend/vulkan/output.inc"
#include "gpu/backend/vulkan/ui.inc"

#if defined(NATIVE_RENDERER_ENGINE_FAULT_TEST)
class Backend::State::Hooks final : public CompletionTestHooks {
 public:
  explicit Hooks(State& state) : state_(state) {}
  bool HoldSubmissions(std::string& evidence) override {
    if (!state_.ready || state_.owner_thread != rex::thread::current_thread_system_id() || !state_.timeline_semaphore) {
      evidence = "{\"held\":false,\"reason\":\"native-vulkan-hold-unavailable\"}";
      return false;
    }
    if (!state_.hold) {
      VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
      type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
      const VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
      const VkResult result = vkCreateSemaphore(state_.vulkan->vk, &info, nullptr, &state_.hold);
      if (result != VK_SUCCESS) {
        evidence = fmt::format("{{\"held\":false,\"reason\":\"native-vulkan-hold-semaphore\",\"vk_result\":{}}}",
                               int32_t(result));
        return false;
      }
    }
    const uint64_t value = state_.hold_signaled.load() + 1;
    state_.hold_value.store(value);
    evidence = fmt::format("{{\"held\":true,\"value\":{}}}", value);
    return true;
  }
  bool ReleaseSubmissions(std::string& evidence) override {
    const uint64_t value = state_.hold_value.load();
    if (!state_.hold || value <= state_.hold_signaled.load()) {
      evidence = "{\"released\":false,\"reason\":\"native-vulkan-hold-not-held\"}";
      return false;
    }
    const VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, nullptr, state_.hold, value};
    const VkResult result = vkSignalSemaphore(state_.vulkan->vk, &signal);
    if (result == VK_SUCCESS) state_.hold_signaled.store(value);
    evidence = fmt::format("{{\"released\":{},\"value\":{},\"vk_result\":{}}}", result == VK_SUCCESS, value,
                           int32_t(result));
    return result == VK_SUCCESS;
  }
  void InjectResult(int32_t result) override { state_.completion->InjectResultForTest(VkResult(result)); }
  void SetDeadline(uint32_t milliseconds) override {
    state_.completion->SetDeadlineForTest(std::chrono::milliseconds(milliseconds));
  }
  uint64_t Submissions() const override { return state_.completion->Submissions(); }

 private:
  State& state_;
};
#endif

bool Backend::State::Submit(VkCommandBuffer commands, VkResult& result, bool& refused) {
  refused = false;
  VkFence fence = VK_NULL_HANDLE;
  if (!completion->Acquire(fence, result)) {
    Observe(result);
    operation_poisoned = true;
    return false;
  }
  const uint64_t sequence = next_sequence;
  auto* queue_native = static_cast<plume::VulkanCommandQueue*>(queue.get())->queue;
  VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  info.commandBufferCount = commands ? 1 : 0;
  info.pCommandBuffers = commands ? &commands : nullptr;
  info.signalSemaphoreCount = submit_signal ? 1 : 0;
  info.pSignalSemaphores = submit_signal ? &submit_signal : nullptr;
#if defined(NATIVE_RENDERER_ENGINE_FAULT_TEST)
  // Held: the submission waits on the GPU, before any of its commands.
  const uint64_t held = hold_value.load();
  const VkPipelineStageFlags hold_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  VkTimelineSemaphoreSubmitInfo hold_info{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
  const uint64_t binary_signal_value = 0;
  if (held > hold_signaled.load()) {
    hold_info.waitSemaphoreValueCount = 1;
    hold_info.pWaitSemaphoreValues = &held;
    hold_info.signalSemaphoreValueCount = info.signalSemaphoreCount;
    hold_info.pSignalSemaphoreValues = &binary_signal_value;
    info.pNext = &hold_info;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &hold;
    info.pWaitDstStageMask = &hold_stage;
  }
#endif
  result = VK_ERROR_UNKNOWN;
  const bool authorized = failures->Authorize([&] {
    unproved_execution = true;
    {
      std::lock_guard lock(*queue_native->mutex);
      result = vkQueueSubmit(queue_native->vk, 1, &info, fence);
    }
    if (result == VK_SUCCESS) completion->Submitted(sequence, fence);
  });
  if (!authorized) {
    completion->Return(fence);
    refused = true;
    return false;
  }
  if (result != VK_SUCCESS) {
    // The fence was not submitted; whether any work ran is unknown.
    completion->Return(fence);
    Observe(result);
    operation_poisoned = true;
    return false;
  }
  unproved_execution = false;
  submit_signal = VK_NULL_HANDLE;
  pending_sequence = sequence;
  ++next_sequence;
  profile::Count(profile::kSubmissions, 1);
  return true;
}

std::unique_ptr<plume::RenderCommandList> Backend::State::BeginList(VkResult& result) {
  std::unique_ptr<plume::RenderCommandList> list;
  if (!free_lists.empty()) {
    list = std::move(free_lists.back());
    free_lists.pop_back();
  } else {
    list = queue->createCommandList();
    if (!list || !static_cast<plume::VulkanCommandList*>(list.get())->vk) {
      result = VK_ERROR_INITIALIZATION_FAILED;
      return nullptr;
    }
    ++lists_created;
  }
  // The pool resets its buffers: beginning one discards what it recorded.
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  result = vkBeginCommandBuffer(static_cast<plume::VulkanCommandList*>(list.get())->vk, &begin);
  Observe(result);
  if (result != VK_SUCCESS) return nullptr;
  return list;
}

VkResult Backend::State::EndList(plume::RenderCommandList& list) {
  if (&list == batch.list.get()) EndDrawPass();
  list.setFramebuffer(nullptr);  // Ends Plume's render pass.
  const VkResult result = vkEndCommandBuffer(static_cast<plume::VulkanCommandList&>(list).vk);
  Observe(result);
  return result;
}

void Backend::State::Reclaim(uint64_t completed) {
  while (!batches_in_flight.empty() && batches_in_flight.front().sequence <= completed) {
    Batch& done = batches_in_flight.front();
    if (done.graphics && free_draw_caches.size() < 4) {
      done.graphics->Recycle();
      free_draw_caches.push_back(std::move(done.graphics));
    }
    free_lists.push_back(std::move(done.list));
    for (auto& page : done.pages)
      if (page.buffer->size == kUploadPageBytes) {
        page.used = 0;
        free_pages.push_back(std::move(page));
      }
    for (uint32_t view : done.views) free_views.push_back(view);
    batches_in_flight.pop_front();
  }
  std::erase_if(readbacks, [&](Readback& r) {
    if (r.sequence > completed) return false;
    free_lists.push_back(std::move(r.list));
    return true;
  });
  std::erase_if(retired_surfaces, [&](const Surface& s) { return s.last_use <= completed; });
  std::erase_if(retired_textures, [&](const Texture& t) {
    if (t.last_use > completed) return false;
    if (t.view) free_views.push_back(t.view);
    ++textures_released;
    return true;
  });
  std::erase_if(retired_sampled, [&](const Sampled& s) {
    if (s.last_use > completed) return false;
    if (s.view) free_views.push_back(s.view);
    ++sampled_released;
    return true;
  });
  std::erase_if(retired_buffers, [&](const Buffer& b) {
    if (b.last_use > completed) return false;
    ++buffers_released;
    return true;
  });
  std::erase_if(retired_ui_textures, [&](const UiTexture& t) { return t.last_use <= completed; });
}

bool Backend::State::OpenBatch(const char*& reason, VkResult& result) {
  result = VK_SUCCESS;
  if (batch_open) return true;
  Reclaim(completion->Completed());
  if (next_sequence == UINT64_MAX) {
    reason = "native-batch-sequence-exhausted";
    return false;
  }
  Batch next;
  next.sequence = next_sequence;
  next.list = BeginList(result);
  if (!next.list) {
    reason = "native-batch-list";
    return false;
  }
  batch = std::move(next);
  batch_open = true;
  return true;
}

bool Backend::State::CloseBatch(const char*& reason, VkResult& result) {
  result = VK_SUCCESS;
  if (!batch_open) return true;
  if (failures->Failed()) {
    reason = "native-batch-refused-after-failure";
    return false;
  }
  profile::Timer submit_time(profile::kEngineSubmit);
  batch_open = false;
  if ((result = EndList(*batch.list)) != VK_SUCCESS) {
    operation_poisoned = true;
    reason = "native-batch-close";
    return false;
  }
  if (batch.sequence != next_sequence) {
    operation_poisoned = true;
    reason = "native-batch-sequence-moved";
    return false;
  }
  // Host writes of non-coherent pages are flushed before the queue reads them.
  for (const auto& page : batch.pages)
    if ((result = vmaFlushAllocation(vulkan->allocator, page.buffer->allocation, 0, page.used)) != VK_SUCCESS) {
      operation_poisoned = true;
      reason = "native-batch-upload-flush";
      return false;
    }
  bool refused = false;
  if (!Submit(Commands(), result, refused)) {
    batch_refused = refused;
    reason = refused ? "native-batch-refused-after-failure" : "native-batch-submit";
    return false;
  }
  ++batches_closed;
  batches_in_flight.push_back(std::move(batch));
  batch = {};
  return true;
}

bool Backend::State::Upload(VkDeviceSize bytes, UploadPage*& page, VkDeviceSize& offset, VkResult& result) {
  result = VK_SUCCESS;
  if (!batch.pages.empty()) {
    auto& current = batch.pages.back();
    const VkDeviceSize at = (current.used + kUploadAlign - 1) & ~(kUploadAlign - 1);
    if (at + bytes <= current.buffer->size) {
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
    const VkDeviceSize size = bytes <= kUploadPageBytes ? kUploadPageBytes : (bytes + 0xFFFF) & ~VkDeviceSize(0xFFFF);
    result = CreateBuffer(vulkan->allocator, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT, next.buffer);
    Observe(result);
    if (result != VK_SUCCESS) return false;
    if (!next.buffer->mapped) {
      result = VK_ERROR_MEMORY_MAP_FAILED;
      return false;
    }
    VkBufferDeviceAddressInfo address{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    address.buffer = next.buffer->buffer;
    next.address = vkGetBufferDeviceAddress(vulkan->vk, &address);
    ++upload_pages_created;
  }
  next.used = bytes;
  batch.pages.push_back(std::move(next));
  page = &batch.pages.back();
  offset = 0;
  return true;
}

bool Backend::State::ReadBack(VkDeviceSize bytes, const std::function<void(VkCommandBuffer, VkBuffer)>& record,
                              uint64_t& sequence, std::shared_ptr<VmaBuffer>& buffer, const char*& reason,
                              VkResult& result) {
  result = CreateBuffer(vulkan->allocator, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT, buffer);
  Observe(result);
  if (result != VK_SUCCESS || !buffer->mapped) {
    reason = "native-read-buffer";
    return false;
  }
  Readback readback;
  readback.list = BeginList(result);
  if (!readback.list) {
    reason = "native-read-list";
    return false;
  }
  const VkCommandBuffer commands = static_cast<plume::VulkanCommandList*>(readback.list.get())->vk;
  record(commands, buffer->buffer);
  VkMemoryBarrier to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0,
                       nullptr, 0, nullptr);
  if ((result = EndList(*readback.list)) != VK_SUCCESS) {
    operation_poisoned = true;
    reason = "native-read-close";
    return false;
  }
  bool refused = false;
  readback.sequence = sequence = next_sequence;
  readback.buffer = buffer;
  if (!Submit(commands, result, refused)) {
    reason = refused ? "native-read-refused-after-failure" : "native-read-submit";
    return false;
  }
  readbacks.push_back(std::move(readback));
  ++operations;
  if (!completion->WaitFor(sequence, kWaitBudget)) {
    reason = "native-read-wait";
    return false;
  }
  pending_sequence = 0;  // Every submission so far completed: the readback was the last.
  Reclaim(completion->Completed());
  if ((result = vmaInvalidateAllocation(vulkan->allocator, buffer->allocation, 0, bytes)) != VK_SUCCESS) {
    reason = "native-read-invalidate";
    return false;
  }
  return true;
}

Backend::Backend(std::shared_ptr<FailureChannel> failures) : state_(std::make_unique<State>()) {
  state_->failures = std::move(failures);
}
Backend::~Backend() {
  std::string ignored;
  // A caller must abort if draining failed. Never implicitly destroy resources
  // referenced by work whose completion has not been proved.
  if (!Shutdown(ignored)) std::_Exit(88);
}

bool Backend::Initialize(std::string& evidence, const RenderMode& mode) {
  auto fail = [&](const char* operation, VkResult result = VK_ERROR_UNKNOWN, const std::string& detail = {}) {
    evidence = fmt::format("{{\"operation\":\"{}\",\"vk_result\":{}{},\"readiness_criteria_met\":false}}", operation,
                           int32_t(result), detail);
    return false;
  };
  if (state_->render) return fail("duplicate-native-initialization");
  if (const char* reason = RenderModeError(mode)) return fail(reason);
  state_->render = plume::CreateVulkanInterface();
  if (!state_->render) return fail("native-vulkan-loader-or-instance-unavailable");
  if (!mode.device.empty()) {
    const auto& names = state_->render->getDeviceNames();
    const auto matches = std::count(names.begin(), names.end(), mode.device);
    if (matches != 1) return fail(matches ? "native-vulkan-device-name-ambiguous" : "native-vulkan-device-not-found",
                                 VK_ERROR_INITIALIZATION_FAILED, fmt::format(",\"requested\":\"{}\"", JsonText(mode.device)));
  }
  state_->device = state_->render->createDevice(mode.device);
  if (!state_->device) return fail("Plume-device");
  auto* vulkan = state_->vulkan = static_cast<plume::VulkanDevice*>(state_->device.get());
  const VkPhysicalDevice physical = vulkan->physicalDevice;
  // What the backend needs of the device: the direct queue, the surface
  // formats and every texture format of the contracts.
  std::vector<std::string> missing;
  uint32_t family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
  std::vector<VkQueueFamilyProperties> families(family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
  const uint32_t direct = vulkan->queueFamilyIndices[0];  // Plume's DIRECT queue.
  const VkQueueFlags direct_needed = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
  if (direct >= family_count || (families[direct].queueFlags & direct_needed) != direct_needed)
    missing.push_back("queue:graphics+compute+transfer");
  auto format_needs = [&](VkFormat format, VkFormatFeatureFlags needed, const char* name) {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
    if ((properties.optimalTilingFeatures & needed) != needed) missing.push_back(std::string("format:") + name);
  };
  constexpr VkFormatFeatureFlags kCopy = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  format_needs(VK_FORMAT_R8G8B8A8_UNORM, kCopy | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
               VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, "R8G8B8A8_UNORM");
  format_needs(VK_FORMAT_D32_SFLOAT_S8_UINT, kCopy | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
               VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, "D32_SFLOAT_S8_UINT");
  format_needs(VK_FORMAT_R32_SFLOAT, kCopy | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
               VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, "R32_SFLOAT");
  for (TextureFormat format : kTextureFormats)
    format_needs(ImageFormat(format), kCopy | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
                 fmt::format("texture-{}", uint32_t(format)).c_str());
  // The SPIR-V contract of the build (its manifest's) against the device.
  std::string contract_error;
  const ShaderPackage* package = EmbeddedShaderPackage(ShaderFormat::kSpirv, contract_error);
  if (!package || !ReadShaderContract(package->index, state_->contract, contract_error))
    return fail("native-vulkan-shader-contract", VK_ERROR_INITIALIZATION_FAILED,
                fmt::format(",\"error\":\"{}\"", contract_error));
  for (auto& name : MissingFromDevice(state_->contract, *vulkan)) missing.push_back(std::move(name));
  if (!missing.empty())
    return fail("native-vulkan-capability", VK_ERROR_FEATURE_NOT_PRESENT,
                fmt::format(",\"missing\":[\"{}\"]", fmt::join(missing, "\",\"")));
  // The MSAA sample count, lowered to one the device supports for both surface formats.
  state_->render_mode = mode;
  for (unsigned i = 0; i < 256; ++i)
    state_->gamma[i] = state_->gamma[256 + i] = state_->gamma[512 + i] = (i * 1023 + 127) / 255;
  constexpr std::pair<VkFormat, VkImageUsageFlags> kSurfaceFormats[] = {
      {VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT},
      {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT}};
  auto msaa_supported = [&](uint32_t samples) {
    for (const auto& [format, usage] : kSurfaceFormats) {
      VkImageFormatProperties properties{};
      if (vkGetPhysicalDeviceImageFormatProperties(physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
              usage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
              0, &properties) != VK_SUCCESS || !(properties.sampleCounts & samples))
        return false;
    }
    return true;
  };
  state_->render_mode.msaa_samples = SupportedSamples(mode.msaa_samples, msaa_supported);
  state_->pipeline_msaa.store(state_->render_mode.msaa_samples);
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
  VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &timeline};
  VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
  VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
  vkGetPhysicalDeviceProperties2(physical, &properties);
  if (properties.properties.apiVersion >= VK_API_VERSION_1_2) vkGetPhysicalDeviceFeatures2(physical, &features);
  state_->timeline_semaphore = timeline.timelineSemaphore;
  state_->queue = state_->device->createCommandQueue(plume::RenderCommandListType::DIRECT);
  if (!state_->queue || !static_cast<plume::VulkanCommandQueue*>(state_->queue.get())->queue)
    return fail("Plume-queue");
  state_->completion = std::make_unique<Completion>(vulkan->vk, state_->failures);
  state_->completion->Start();
  // Only host readiness: a real completion of the queue, never published as a
  // guest fence, read pointer or draw completion.
  VkResult result = VK_SUCCESS;
  bool refused = false;
  const uint64_t signal = state_->next_sequence;
  if (!state_->Submit(VK_NULL_HANDLE, result, refused))
    return fail(refused ? "native-readiness-refused" : "native-queue-signal", result);
  if (!state_->completion->WaitFor(signal, kWaitBudget)) return fail("native-readiness-wait", VK_TIMEOUT);
  state_->pending_sequence = 0;
  state_->epoch = AllocateEngineEpoch();
  if (state_->epoch == 0) return fail("native-owner-epoch-exhausted");
  state_->owner_thread = rex::thread::current_thread_system_id();
  state_->ready = true;
  const auto& p = vulkan->physicalDeviceProperties;
  std::string requested;
  if (!mode.device.empty())
    requested = fmt::format(",\"device_requested\":\"{}\",\"device_matched\":{}", JsonText(mode.device),
                            state_->device->getDescription().name == mode.device);
  evidence = fmt::format("{{\"backend\":\"Plume/Vulkan\",\"device_vendor\":{},\"device_id\":{},"
      "\"device_name\":\"{}\",\"device_api_version\":\"{}\",\"driver_version\":{},\"driver_id\":{},"
      "\"driver_name\":\"{}\",\"driver_info\":\"{}\",\"direct_queue_family\":{},\"timeline_semaphore\":{},"
      "\"descriptor_indexing\":{},\"buffer_device_address\":{},\"null_descriptor\":{},"
      "\"rgba8_render_blend\":true,\"r8_sample\":true,\"direct_queue\":true,\"sequence_signal\":{},"
      "\"sequence_completed\":{},\"shader_contract\":{{\"sha256\":\"{}\",\"capabilities\":{},\"extensions\":{},"
      "\"bindings\":{},\"push_ranges\":{}}},\"validation\":\"loader-layer-log\",\"guest_completion\":false,"
      "\"presentation\":false{}{}}}", p.vendorID, p.deviceID, JsonText(p.deviceName), Version(p.apiVersion),
      p.driverVersion, int32_t(driver.driverID), JsonText(driver.driverName), JsonText(driver.driverInfo), direct,
      state_->timeline_semaphore, vulkan->capabilities.descriptorIndexing, vulkan->capabilities.bufferDeviceAddress,
      vulkan->nullDescriptorSupported, signal, state_->completion->Completed(), state_->contract.sha256,
      state_->contract.capabilities.size(), state_->contract.extensions.size(), state_->contract.bindings.size(),
      state_->contract.push_ranges.size(),
      // Only a mode other than the default is recorded.
      mode.scale.Unit() && !mode.msaa_samples ? std::string() : fmt::format(
          ",\"render_mode\":{{\"scale\":[{},{}],\"msaa_samples\":{},\"msaa_requested\":{}}}", mode.scale.x,
          mode.scale.y, state_->render_mode.msaa_samples, mode.msaa_samples), requested);
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
  if (state_->ready && state_->owner_thread != rex::thread::current_thread_system_id()) {
    evidence = "{\"released\":false,\"reason\":\"native-owner-thread\",\"guest_shutdown\":false}";
    return false;
  }
  if (state_->unproved_execution) {
    evidence = "{\"released\":false,\"reason\":\"executed-work-without-completion-marker\","
               "\"guest_shutdown\":false}";
    return false;
  }
  state_->pipeline_set.Stop();  // The asynchronous pipeline threads end before anything is released.
  // Everything submitted must be seen complete before anything is released;
  // unproved, ownership is kept for the diagnostic process termination.
  std::string completion = "null";
  uint64_t completed = 0;
  if (state_->completion) {
    const uint64_t submitted = state_->next_sequence - 1;
    const bool done = state_->completion->Completed() >= submitted ||
                      state_->completion->WaitFor(submitted, kWaitBudget);
    completed = state_->completion->Completed();
    if (!done || state_->device_lost) {
      evidence = fmt::format("{{\"released\":false,\"submitted_sequence\":{},\"observed_completed\":{},"
          "\"device_lost\":{},\"reason\":\"native-completion-unproved-abort-required\",\"completion\":{{{}}},"
          "\"guest_shutdown\":false}}", submitted, completed, state_->device_lost, state_->completion->Summary());
      return false;
    }
    state_->completion->Stop();
    completion = "{" + state_->completion->Summary() + "}";
  }
  // An open batch was never submitted; it is discarded, not claimed complete.
  const bool open_batch_discarded = state_->batch_open || state_->batch_refused;
  const uint64_t open_batch_operations = open_batch_discarded ? state_->batch.operations : 0;
  state_->batch_open = state_->batch_refused = false;
  state_->batch = {};
  state_->batches_in_flight.clear();
  state_->free_draw_caches.clear();
  state_->readbacks.clear();
  state_->free_lists.clear();
  state_->free_pages.clear();
  if (!state_->RetireSwapchain(evidence)) return false;
  state_->ReleaseOutput();
  state_->retired_surfaces.clear();  // Their completion was proved above.
  state_->retired_textures.clear();
  state_->retired_sampled.clear();
  state_->retired_buffers.clear();
  state_->pipeline_set.Clear();
  state_->ReleaseUi();
  state_->ReleaseGraphics();
  state_->surfaces.clear();
  state_->textures.clear();
  state_->sampled.clear();
  state_->buffers.clear();
  state_->free_views.clear();
  state_->next_view = 1;
  state_->completion.reset();  // Its fences, before the device.
#if defined(NATIVE_RENDERER_ENGINE_FAULT_TEST)
  if (state_->hold) vkDestroySemaphore(state_->vulkan->vk, state_->hold, nullptr);
  state_->hold = VK_NULL_HANDLE;
#endif
  state_->queue.reset();
  state_->device.reset();
  state_->vulkan = nullptr;
  state_->render.reset();
  state_->ready = false;
  state_->operation_poisoned = false;
  state_->owner_thread = 0;
  state_->epoch = 0;
  const auto failure = state_->failures->First();
  evidence = fmt::format("{{\"completed_sequence\":{},\"batches_closed\":{},\"open_batch_discarded\":{},"
      "\"open_batch_operations\":{},\"command_lists\":{},\"upload_pages\":{},\"posted_failure_sequence\":{},"
      "\"completion\":{},\"validation\":\"loader-layer-log\",\"released\":[\"completion-fences\",\"queue\","
      "\"device\",\"interface\"],\"guest_shutdown\":false}}", completed, state_->batches_closed,
      open_batch_discarded, open_batch_operations, state_->lists_created, state_->upload_pages_created,
      failure ? failure->sequence : 0, completion);
  return true;
}

bool Backend::InitializeFrontend(uint64_t generation, std::string& evidence) {
  // No admitted operation, resource generation or GPU callback is discarded.
  if (!state_->ready || state_->owner_thread != rex::thread::current_thread_system_id() || state_->operation_poisoned ||
      state_->pending_sequence || state_->unproved_execution || !state_->frontend.Initialize(generation)) {
    evidence = "{\"reason\":\"frontend-not-cold-or-duplicate\"}";
    return false;
  }
  return VerifyFrontend(generation, evidence);
}

bool Backend::VerifyFrontend(uint64_t generation, std::string& evidence) {
  if (!state_->ready || state_->owner_thread != rex::thread::current_thread_system_id() || !state_->device) {
    evidence = "{\"reason\":\"frontend-generation-idle-or-device-unproved\",\"failed\":[\"owner\"]}";
    return false;
  }
  // Each failed condition is named.
  std::string failed;
  auto check = [&](bool ok, const char* name) {
    if (!ok) failed += fmt::format("{}\"{}\"", failed.empty() ? "" : ",", name);
  };
  check(state_->frontend.Matches(generation) && !state_->frontend.CanConsume(), "frontend-generation");
  check(!state_->operation_poisoned, "operation-poisoned");
  check(!state_->pending_sequence, "pending-sequence");
  check(!state_->unproved_execution, "unproved-execution");
  check(!state_->device_lost, "device-lost");
  if (!failed.empty()) {
    evidence = fmt::format("{{\"reason\":\"frontend-generation-idle-or-device-unproved\",\"failed\":[{}],"
                           "\"pending_sequence\":{},\"device_lost\":{}}}", failed, state_->pending_sequence,
                           state_->device_lost);
    return false;
  }
  evidence = FrontendReadyEvidence(generation, state_->epoch, rex::thread::current_thread_system_id());
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
  if (!state_->ready || state_->owner_thread != rex::thread::current_thread_system_id() || state_->operation_poisoned)
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
  State::Surface surface;
  surface.desc = desc;
  surface.guest = guest;
  surface.texture = state_->device->createTexture(depth ?
      plume::RenderTextureDesc::DepthTarget(desc.width, desc.height, plume::RenderFormat::D32_FLOAT_S8_UINT, samples) :
      plume::RenderTextureDesc::ColorTarget(desc.width, desc.height, plume::RenderFormat::R8G8B8A8_UNORM, samples));
  if (!surface.texture || !surface.Image()) return fail("native-surface-create");
  const plume::RenderTexture* attachment[] = {surface.texture.get()};
  surface.framebuffer = state_->device->createFramebuffer(depth ?
      plume::RenderFramebufferDesc(nullptr, 0, surface.texture.get()) : plume::RenderFramebufferDesc(attachment, 1));
  if (!surface.framebuffer || !static_cast<plume::VulkanFramebuffer*>(surface.framebuffer.get())->vk)
    return fail("native-surface-framebuffer");
  state_->surfaces.emplace(desc.key, std::move(surface));
  evidence = fmt::format("{{\"created\":true,\"key\":{},\"width\":{},\"height\":{},\"guest_format\":{},"
      "\"host_format\":\"{}\",\"samples\":{},\"live_surfaces\":{}}}", desc.key, desc.width, desc.height,
      desc.guest_format, depth ? "D32_SFLOAT_S8_UINT" : "R8G8B8A8_UNORM", desc.samples, state_->surfaces.size());
  return true;
}

bool Backend::Reconfigure(const RenderMode& requested, const PresentMode& present, std::string& evidence) {
  RenderMode render = requested;
  auto& s = *state_;
  if (!s.ready || s.owner_thread != rex::thread::current_thread_system_id() || s.operation_poisoned || s.unproved_execution)
    return GraphicsError(evidence, "native-reconfigure-owner-or-state");
  if (!ValidLiveModes(s.render_mode, render, present)) return GraphicsError(evidence, "native-reconfigure-mode");
  render.msaa_samples = SupportedSamples(requested.msaa_samples, [&](uint32_t samples) {
    for (const auto& [format, usage] : {
      std::pair{VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT},
      std::pair{VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT}}) {
    VkImageFormatProperties props{};
    if (vkGetPhysicalDeviceImageFormatProperties(s.vulkan->physicalDevice, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        usage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &props) != VK_SUCCESS ||
        !(props.sampleCounts & samples)) return false;
    }
    return true;
  });
  const bool targets = render.scale.x != s.render_mode.scale.x || render.scale.y != s.render_mode.scale.y ||
                       render.msaa_samples != s.render_mode.msaa_samples;
  for (const auto& [key, surface] : s.surfaces) {
    const auto desc = render.Host(surface.guest);
    if (!desc.width || !desc.height || desc.width > 8192 || desc.height > 8192)
      return GraphicsError(evidence, "native-reconfigure-surface-size");
  }
  if (!WaitIdle(evidence)) return false;
  const auto old_render = s.render_mode;
  const auto old_present = s.present_mode;
  decltype(s.surfaces) old_surfaces;
  if (targets) {
    old_surfaces.swap(s.surfaces); s.render_mode = render;
    for (const auto& [key, surface] : old_surfaces) if (!CreateSurface(surface.guest, evidence)) {
      s.surfaces.clear(); s.surfaces.swap(old_surfaces); s.render_mode = old_render;
      return false;
    }
  }
  const bool swap = present.vsync != old_present.vsync || present.allow_tearing != old_present.allow_tearing ||
                    present.buffers != old_present.buffers;
  s.present_mode = present;
  if (swap && s.window.kind != platform::NativeWindow::Kind::kNone) {
    uint32_t width = 0, height = 0;
    if (platform::ClientSize(s.window, width, height)) {
      if (!s.CreateSwapchain(width, height, evidence)) {
        const auto error = evidence;
        s.present_mode = old_present;
        std::string rollback;
        if (!s.CreateSwapchain(width, height, rollback)) s.operation_poisoned = true;
        if (targets) { s.surfaces.clear(); s.surfaces.swap(old_surfaces); s.render_mode = old_render; }
        evidence = error; return false;
      }
    } else s.recreate_swapchain = true;
  }
  s.render_mode = render;
  s.pipeline_msaa.store(render.msaa_samples);
  evidence = fmt::format("{{\"applied\":true,\"targets_rebuilt\":{},\"surfaces\":{},\"resolved_textures_retained\":{},"
      "\"scale\":[{},{}],\"msaa_samples\":{},\"vsync\":{},\"brightness\":{},\"buffers\":{},\"effective_frame_latency\":1,"
      "\"effect\":{},\"post_effect\":{}}}", targets, s.surfaces.size(), s.textures.size(), render.scale.x,
      render.scale.y, render.msaa_samples, present.vsync, present.brightness, present.buffers,
      uint32_t(present.effect), uint32_t(present.post_effect));
  return true;
}

bool Backend::EnsureSurface(const SurfaceDesc& desc, std::string& evidence) {
  if (!state_->ready || state_->owner_thread != rex::thread::current_thread_system_id()) {
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
    if (!DestroySurface(desc.key, destroyed)) {
      evidence = destroyed;
      return false;
    }
  }
  return CreateSurface(desc, evidence);
}

bool Backend::DestroySurface(uint32_t key, std::string& evidence) {
  if (!state_->ready || state_->owner_thread != rex::thread::current_thread_system_id()) {
    evidence = "{\"destroyed\":false,\"reason\":\"native-surface-owner\"}";
    return false;
  }
  auto it = state_->surfaces.find(key);
  if (it == state_->surfaces.end()) {
    evidence = fmt::format("{{\"destroyed\":false,\"reason\":\"native-surface-unknown\",\"key\":{}}}", key);
    return false;
  }
  // A surface still referenced by submitted or recorded work is retired, not
  // destroyed: it is released once its sequence completes.
  const uint64_t completed = state_->completion->Completed();
  const uint64_t last_use = it->second.last_use;
  const bool in_flight = state_->InFlight(last_use);
  if (in_flight) state_->retired_surfaces.push_back(std::move(it->second));
  state_->surfaces.erase(it);
  state_->Reclaim(completed);
  evidence = fmt::format("{{\"destroyed\":true,\"key\":{},\"deferred\":{},\"last_use_fence\":{},"
      "\"observed_completed\":{},\"live_surfaces\":{},\"retired_surfaces\":{}}}", key, in_flight, last_use,
      completed, state_->surfaces.size(), state_->retired_surfaces.size());
  return true;
}

bool Backend::ClearSurfaces(const ClearRequest& request, uint64_t& fence, std::string& evidence) {
  profile::Timer profile(profile::kEngineClear);
  fence = 0;
  auto fail = [&](const char* reason, VkResult result = VK_ERROR_UNKNOWN) {
    evidence = fmt::format("{{\"cleared\":false,\"reason\":\"{}\",\"vk_result\":{}}}", reason, int32_t(result));
    return false;
  };
  if (!state_->ready || state_->owner_thread != rex::thread::current_thread_system_id() || state_->operation_poisoned ||
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
  // Recorded into the open batch, complete with its sequence. Each surface is
  // cleared in a render pass of its own framebuffer.
  VkResult result = VK_SUCCESS;
  const char* batch_reason = nullptr;
  if (!state_->OpenBatch(batch_reason, result)) return fail(batch_reason, result);
  plume::RenderCommandList& list = *state_->batch.list;
  const VkCommandBuffer commands = state_->Commands();
  for (auto* surface : colors) {
    HostRect rect;
    Transition(commands, surface->Image(), surface->Aspect(), surface->state, kColorTarget);
    if (ClearRect(state_->render_mode.scale, request, surface->desc.width, surface->desc.height, rect)) {
      const plume::RenderRect clear(rect.left, rect.top, rect.right, rect.bottom);
      list.setFramebuffer(surface->framebuffer.get());
      list.clearColor(0, plume::RenderColor(request.color[0], request.color[1], request.color[2], request.color[3]),
                      &clear, 1);
      list.setFramebuffer(nullptr);
    }
  }
  if (depth) {
    HostRect rect;
    Transition(commands, depth->Image(), depth->Aspect(), depth->state, kDepthTarget);
    if (ClearRect(state_->render_mode.scale, request, depth->desc.width, depth->desc.height, rect)) {
      const plume::RenderRect clear(rect.left, rect.top, rect.right, rect.bottom);
      list.setFramebuffer(depth->framebuffer.get());
      list.clearDepthStencil(request.clear_depth, request.clear_stencil, request.depth, request.stencil, &clear, 1);
      list.setFramebuffer(nullptr);
    }
  }
  for (auto* surface : colors) surface->last_use = state_->batch.sequence;
  if (depth) depth->last_use = state_->batch.sequence;
  fence = state_->batch.sequence;
  ++state_->batch.operations;
  ++state_->operations;
  evidence = fmt::format("{{\"cleared\":true,\"fence\":{},\"color_surfaces\":{},\"depth\":{},"
      "\"color\":[{},{},{},{}],\"z\":{},\"stencil\":{},\"rect\":[{},{},{},{}],\"operations\":{}}}",
      fence, colors.size(), depth != nullptr, request.color[0], request.color[1], request.color[2],
      request.color[3], request.depth, request.stencil, request.left, request.top, request.right, request.bottom,
      state_->operations);
  return true;
}

bool Backend::WaitIdle(std::string& evidence) { return Wait(true, evidence); }
bool Backend::WaitSubmitted(std::string& evidence) { return Wait(false, evidence); }
bool Backend::Wait(bool close, std::string& evidence) {
  profile::Timer profile(profile::kEngineWaitIdle);
  if (!state_->ready || state_->owner_thread != rex::thread::current_thread_system_id()) {
    evidence = "{\"idle\":false,\"reason\":\"native-idle-owner\"}";
    return false;
  }
  const char* batch_reason = nullptr;
  VkResult batch_result = VK_SUCCESS;
  if (close && !state_->CloseBatch(batch_reason, batch_result)) {
    evidence = fmt::format("{{\"idle\":false,\"reason\":\"{}\",\"vk_result\":{}}}", batch_reason,
                           int32_t(batch_result));
    return false;
  }
  const uint64_t target = state_->pending_sequence;
  const bool waited = !target || state_->completion->WaitFor(target, kWaitBudget);
  const uint64_t completed = state_->completion->Completed();
  const bool ok = waited && completed >= target && !state_->device_lost;
  if (ok) {
    state_->pending_sequence = 0;  // Every submitted operation is complete.
    state_->Reclaim(completed);
  }
  evidence = fmt::format("{{\"idle\":{},\"target\":{},\"observed_completed\":{},\"waited\":{},\"device_lost\":{},"
      "\"batches_closed\":{},\"command_lists\":{},\"upload_pages\":{}}}", ok, target, completed, waited,
      state_->device_lost, state_->batches_closed, state_->lists_created, state_->upload_pages_created);
  return ok;
}

// Frame lot of D3DDevice_Swap. Closes the open batch without waiting and has
// the completion service set the host event when the last submission
// completes, so every operation submitted so far is done when it fires.
bool Backend::CloseFrame(FrameEvent event, uint64_t& target, std::string& evidence) {
  target = 0;
  auto fail = [&](const char* reason, VkResult result = VK_ERROR_UNKNOWN) {
    evidence = fmt::format("{{\"closed\":false,\"reason\":\"{}\",\"vk_result\":{}}}", reason, int32_t(result));
    return false;
  };
  if (!state_->ready || state_->owner_thread != rex::thread::current_thread_system_id() || !event || state_->operation_poisoned ||
      state_->unproved_execution) return fail("native-frame-owner-or-state");
  const char* batch_reason = nullptr;
  VkResult result = VK_SUCCESS;
  if (!state_->CloseBatch(batch_reason, result)) return fail(batch_reason, result);
  state_->pipeline_set.EndFrame();
  target = state_->next_sequence - 1;  // The last submission.
  const uint64_t completed = state_->completion->Completed();
  if (!state_->completion->SetEventOnCompletion(target, std::move(event)))
    return fail("native-frame-event");
  evidence = fmt::format("{{\"closed\":true,\"target\":{},\"completed\":{},\"batches_closed\":{}}}", target,
                         completed, state_->batches_closed);
  return true;
}

std::string Backend::ResourceCounts() {
  uint32_t textures = 0;
  for (const auto& [key, texture] : state_->textures) textures += texture.texture != nullptr;
  return fmt::format("{{\"textures\":{},\"texture_entries\":{},\"sampled\":{},\"surfaces\":{},\"retired_surfaces\":{},"
                     "\"retired_textures\":{},\"textures_released\":{},\"textures_retired\":{},\"retired_sampled\":{},"
                     "\"sampled_released\":{},\"sampled_retired\":{},\"views_allocated\":{},\"views_free\":{},"
                     "\"stage_slots\":{},\"swapchain\":{},\"buffers\":{},\"retired_buffers\":{},"
                     "\"buffers_created\":{},\"buffers_released\":{},\"buffers_retired\":{},\"buffer_uploads\":{},"
                     "\"readbacks\":{},\"batches_in_flight\":{},\"depth_resolves\":{},\"ui_textures\":{},"
                     "\"retired_ui_textures\":{},\"ui_texture_uploads\":{},\"ui_frames_drawn\":{},"
                     "\"ui_draws_skipped\":{}}}",
                     textures, state_->textures.size(), state_->sampled.size(), state_->surfaces.size(),
                     state_->retired_surfaces.size(), state_->retired_textures.size(), state_->textures_released,
                     state_->textures_retired, state_->retired_sampled.size(), state_->sampled_released,
                     state_->sampled_retired, state_->next_view - 1, state_->free_views.size(),
                     std::count_if(state_->stages.begin(), state_->stages.end(), [](const auto& s) { return bool(s.image); }),
                     state_->swapchain != VK_NULL_HANDLE,
                     state_->buffers.size(), state_->retired_buffers.size(), state_->buffers_created,
                     state_->buffers_released, state_->buffers_retired, state_->buffer_uploads,
                     state_->readbacks.size(), state_->batches_in_flight.size(), state_->depth_resolves,
                     state_->ui_textures.size() - state_->ui_textures.count(0), state_->retired_ui_textures.size(),
                     state_->ui_texture_uploads, state_->ui_frames_drawn, state_->ui_draws_skipped);
}

std::string Backend::PipelineSummary() {
  const auto& c = state_->draw_counters;
  auto summary = state_->pipeline_set.Summary() + fmt::format(
      ",\"vulkan_draws\":{},\"draw_descriptor_pools\":{},\"draw_descriptor_groups\":{},\"draw_image_views\":{},"
      "\"draw_samplers\":{},\"draw_framebuffers\":{},\"draw_constant_bytes\":{},\"draw_render_passes\":{},"
      "\"draw_pipeline_binds\":{},\"draw_descriptor_binds\":{},\"draw_set_allocations\":{}",
      c.draws.load(), c.pools.load(), c.groups.load(), c.views.load(), c.samplers.load(),
      c.framebuffers.load(), c.constant_bytes.load(), c.passes.load(), c.pipeline_binds.load(), c.descriptor_binds.load(),
      c.set_allocations.load());
#if defined(NATIVE_RENDERER_ENGINE_FAULT_TEST)
  summary += fmt::format(",\"draw_descriptor_elements\":{}", c.descriptor_elements.load());
#endif
  return summary;
}

bool Backend::LiveResources(ResourceKeys& keys, std::string& evidence) {
  if (!state_->ready || state_->owner_thread != rex::thread::current_thread_system_id()) {
    evidence = "{\"released\":false,\"reason\":\"native-release-remaining-owner\"}";
    return false;
  }
  for (const auto& [key, surface] : state_->surfaces) keys.surfaces.push_back(key);
  for (const auto& [key, texture] : state_->textures) keys.textures.push_back(key);
  for (const auto& [address, copy] : state_->sampled) keys.sampled.push_back(address);
  for (const auto& [key, buffer] : state_->buffers) keys.buffers.push_back(key);
  std::sort(keys.surfaces.begin(), keys.surfaces.end());
  std::sort(keys.textures.begin(), keys.textures.end());
  std::sort(keys.sampled.begin(), keys.sampled.end());
  std::sort(keys.buffers.begin(), keys.buffers.end());
  return true;
}

bool Backend::ReleaseTexture(uint32_t key, std::string& evidence) {
  if (!state_->ready || state_->owner_thread != rex::thread::current_thread_system_id()) {
    evidence = "{\"released\":false,\"reason\":\"native-texture-release-owner\"}";
    return false;
  }
  auto it = state_->textures.find(key);
  if (it == state_->textures.end()) {
    evidence = fmt::format("{{\"released\":true,\"key\":{},\"own_texture\":false}}", key);
    return true;
  }
  // The open batch has not been submitted: its sequence is above every completed one.
  const uint64_t completed = state_->completion->Completed();
  const uint64_t last_use = it->second.last_use;
  const bool in_flight = state_->InFlight(last_use);
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
  if (!state_->ready || state_->owner_thread != rex::thread::current_thread_system_id()) {
    evidence = "{\"released\":false,\"reason\":\"native-sampled-release-owner\"}";
    return false;
  }
  auto it = state_->sampled.find(address);
  if (it == state_->sampled.end()) {
    evidence = fmt::format("{{\"released\":true,\"address\":{},\"sampled\":false}}", address);
    return true;
  }
  const uint64_t completed = state_->completion->Completed();
  const uint64_t last_use = it->second.last_use;
  const bool in_flight = state_->InFlight(last_use);
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
                         "\"observed_completed\":{},\"view\":{},\"counts\":{}}}", address, in_flight, last_use,
                         completed, view, ResourceCounts());
  return true;
}

bool Backend::ResolveSurface(const ResolveRequest& request, uint64_t& fence, std::string& evidence) {
  profile::Timer profile(profile::kEngineResolve);
  fence = 0;
  auto fail = [&](const char* reason, VkResult result = VK_ERROR_UNKNOWN) {
    evidence = fmt::format("{{\"resolved\":false,\"reason\":\"{}\",\"vk_result\":{}}}", reason, int32_t(result));
    return false;
  };
  if (!state_->ready || state_->owner_thread != rex::thread::current_thread_system_id() || state_->operation_poisoned ||
      state_->unproved_execution) return fail("native-resolve-owner-or-state");
  auto source_it = state_->surfaces.find(request.source_key);
  if (source_it == state_->surfaces.end()) return fail("native-resolve-source-unknown");
  auto& source = source_it->second;
  const bool depth = source.desc.format == SurfaceFormat::DepthStencil;
  if (!request.dest_key || !request.dest_width || !request.dest_height) return fail("native-resolve-destination");
  // The request is in guest pixels; the destination is as scaled as the source.
  const HostResolve host = ResolveInHost(state_->render_mode.scale, request);
  // One sample of an MSAA color source goes through the pass, as the depth does.
  const bool sampled = !depth && source.desc.samples > 1 && request.sample >= 0;
  // Own destination texture keyed by the guest texture header; recreated when
  // the header describes another size, or when a pass needs it as a render target.
  auto& dest = state_->textures[request.dest_key];
  if (!dest.texture || dest.width != host.dest_width || dest.height != host.dest_height || dest.depth != depth ||
      (sampled && !dest.render_target)) {
    // Recreated once no submitted or recorded work can still use it.
    if (dest.texture && state_->InFlight(dest.last_use)) return fail("native-resolve-destination-in-flight");
    const uint32_t view = dest.view;  // The draw rewrites the descriptor for the new image.
    dest = {};
    dest.view = view;
    // A depth destination, or the one of a color sample, is the render target of its pass.
    dest.texture = state_->device->createTexture(plume::RenderTextureDesc::Texture2D(
        host.dest_width, host.dest_height, 1,
        depth ? plume::RenderFormat::R32_FLOAT : plume::RenderFormat::R8G8B8A8_UNORM,
        depth || sampled ? plume::RenderTextureFlag::RENDER_TARGET : plume::RenderTextureFlag::NONE));
    if (!dest.texture || !dest.Image()) return fail("native-resolve-destination-create");
    dest.width = host.dest_width;
    dest.height = host.dest_height;
    dest.depth = depth;
    dest.render_target = depth || sampled;
  }
  const bool same_size = source.desc.width == dest.width && source.desc.height == dest.height;
  // The deferred cases (content left undefined with success) are refused:
  // the draw layer stops in a guard before reaching them.
  if (const char* uncovered = ResolveUncovered(request, depth, source.desc.samples, same_size)) return fail(uncovered);
  HostBox box;
  if (!depth && source.desc.samples == 1 &&
      !CopyBox(host, request.has_rect, source.desc.width, source.desc.height, dest.width, dest.height, box))
    return fail("native-resolve-empty-region");
  // Recorded into the open batch, complete with its sequence.
  VkResult result = VK_SUCCESS;
  const char* batch_reason = nullptr;
  if (!state_->OpenBatch(batch_reason, result)) return fail(batch_reason, result);
  const VkCommandBuffer commands = state_->Commands();
  const char* method = "copy-region";
  if (depth || sampled) {
    if (!state_->ResolvePass(source, dest, request.sample, evidence)) return false;
    method = depth ? (source.desc.samples == 1 ? "depth-pass-copy" : request.sample < 0 ?
        "depth-pass-min" : "depth-pass-sample") : "color-pass-sample";
    if (depth) ++state_->depth_resolves;
  } else if (source.desc.samples > 1) {
    // The average of the samples into the whole destination (same size, no rectangle).
    Transition(commands, source.Image(), source.Aspect(), source.state, kCopySource);
    Transition(commands, dest.Image(), VK_IMAGE_ASPECT_COLOR_BIT, dest.state, kCopyDest, true);
    // With the half-pixel offset gap (regions.h) filled.
    std::vector<VkImageResolve> regions;
    for (const GapCopy& g : ResolveCopies(state_->render_mode.scale, {0, 0, dest.width, dest.height}, 0, 0)) {
      VkImageResolve& region = regions.emplace_back();
      region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.srcOffset = {int32_t(g.source.left), int32_t(g.source.top), 0};
      region.dstOffset = {int32_t(g.dest_x), int32_t(g.dest_y), 0};
      region.extent = {g.source.right - g.source.left, g.source.bottom - g.source.top, 1};
    }
    vkCmdResolveImage(commands, source.Image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest.Image(),
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()), regions.data());
    method = "resolve-image";
  } else {
    Transition(commands, source.Image(), source.Aspect(), source.state, kCopySource);
    Transition(commands, dest.Image(), VK_IMAGE_ASPECT_COLOR_BIT, dest.state, kCopyDest);
    // With the half-pixel offset gap (regions.h) filled.
    std::vector<VkImageCopy> regions;
    for (const GapCopy& g : ResolveCopies(state_->render_mode.scale, box, host.dest_x, host.dest_y)) {
      VkImageCopy& region = regions.emplace_back();
      region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.srcOffset = {int32_t(g.source.left), int32_t(g.source.top), 0};
      region.dstOffset = {int32_t(g.dest_x), int32_t(g.dest_y), 0};
      region.extent = {g.source.right - g.source.left, g.source.bottom - g.source.top, 1};
    }
    vkCmdCopyImage(commands, source.Image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest.Image(),
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()), regions.data());
  }
  source.last_use = dest.last_use = state_->batch.sequence;
  dest.content_defined = true;
  fence = state_->batch.sequence;
  ++state_->batch.operations;
  ++state_->operations;
  evidence = fmt::format("{{\"resolved\":true,\"method\":\"{}\",\"fence\":{},\"source\":{},\"dest\":{},"
      "\"source_size\":[{},{}],\"dest_size\":[{},{}],\"samples\":{},\"operations\":{}}}", method, fence,
      request.source_key, request.dest_key, source.desc.width, source.desc.height, dest.width, dest.height,
      source.desc.samples, state_->operations);
  return true;
}

bool Backend::UploadSampled(const DrawTexture& t, std::string& evidence) {
  profile::Timer profile(profile::kEngineTexture);
  auto fail = [&](const char* reason, VkResult result = VK_ERROR_UNKNOWN) {
    evidence = fmt::format("{{\"uploaded\":false,\"reason\":\"{}\",\"key\":{},\"vk_result\":{}}}", reason, t.key,
                           int32_t(result));
    return false;
  };
  if (!state_->ready || state_->owner_thread != rex::thread::current_thread_system_id() || state_->operation_poisoned ||
      state_->unproved_execution) return fail("native-upload-owner-or-state");
  const uint32_t subresources = t.levels * (t.cube ? 6 : 1);
  if (t.resolved || !t.width || !t.height || !t.levels || t.levels > 13 || t.width > 8192 || t.height > 8192 ||
      t.data.size() != subresources) return fail("native-upload-texture-description");
  const VkFormat format = ImageFormat(t.format);
  if (format == VK_FORMAT_UNDEFINED) return fail("native-upload-texture-create", VK_ERROR_FORMAT_NOT_SUPPORTED);
  // Subresource i is level i % levels of face i / levels, its rows packed.
  VkDeviceSize bytes = 0;
  std::vector<VkDeviceSize> offsets(subresources);
  for (uint32_t i = 0; i < subresources; ++i) {
    const LevelRows rows = RowsOfLevel(t.format, t.width, t.height, i % t.levels);
    if (t.data[i].size() != size_t(rows.rows) * rows.row_bytes) return fail("native-upload-texture-data-size");
    offsets[i] = bytes;
    bytes += (VkDeviceSize(t.data[i].size()) + 15) & ~VkDeviceSize(15);  // A block of any format.
  }
  VkResult result = VK_SUCCESS;
  const char* batch_reason = nullptr;
  if (!state_->OpenBatch(batch_reason, result)) return fail(batch_reason, result);
  auto& s = state_->sampled[t.key];
  const bool recreate = !s.image || s.layout != t.layout || s.format != t.format || s.width != t.width ||
                        s.height != t.height || s.levels != t.levels || s.cube != t.cube;
  if (!recreate && s.version >= t.version) {
    evidence = fmt::format("{{\"uploaded\":false,\"skipped\":true,\"key\":{},\"version\":{},\"copy_version\":{}}}",
                           t.key, t.version, s.version);
    return true;
  }
  if (recreate) {
    // An image the submitted or recorded work may still read stays with the
    // open batch, and so does its view slot.
    if (s.image && state_->InFlight(s.last_use)) {
      state_->batch.retained.push_back(s.image);
      if (s.view) state_->batch.views.push_back(s.view);
      s.view = 0;
    }
    s.image.reset();
    {
      profile::Timer create_time(profile::kEngineTextureCreate);
      result = CreateImage(state_->vulkan->allocator, format, t.width, t.height, t.levels, t.cube, s.image);
    }
    state_->Observe(result);
    if (result != VK_SUCCESS) {
      state_->sampled.erase(t.key);
      return fail("native-upload-texture-create", result);
    }
    s.state = {};
    s.format = t.format;
    s.width = t.width;
    s.height = t.height;
    s.levels = t.levels;
    s.layout = t.layout;
    s.cube = t.cube;
    s.last_use = 0;
  }
  profile::Count(profile::kUploadBytes, bytes);
  State::UploadPage* page = nullptr;
  VkDeviceSize page_offset = 0;
  if (!state_->Upload(bytes, page, page_offset, result)) return fail("native-upload-buffer", result);
  std::vector<VkBufferImageCopy> regions(subresources);
  for (uint32_t i = 0; i < subresources; ++i) {
    std::memcpy(page->buffer->mapped + page_offset + offsets[i], t.data[i].data(), t.data[i].size());
    const uint32_t level = i % t.levels;
    regions[i].bufferOffset = page_offset + offsets[i];
    regions[i].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, i / t.levels, 1};
    regions[i].imageExtent = {std::max(t.width >> level, 1u), std::max(t.height >> level, 1u), 1};
  }
  // Every level of every face is written: the previous content is not kept.
  const VkCommandBuffer commands = state_->Commands();
  Transition(commands, s.image->image, VK_IMAGE_ASPECT_COLOR_BIT, s.state, kCopyDest, true);
  vkCmdCopyBufferToImage(commands, page->buffer->buffer, s.image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         subresources, regions.data());
  Transition(commands, s.image->image, VK_IMAGE_ASPECT_COLOR_BIT, s.state, kShaderRead);
  s.version = t.version;
  s.last_use = state_->batch.sequence;
  const uint64_t batch_operations = ++state_->batch.operations;
  ++state_->operations;
  if (batch_operations >= kBatchOperationsMax && !state_->CloseBatch(batch_reason, result))
    return fail(batch_reason, result);
  evidence = fmt::format("{{\"uploaded\":true,\"key\":{},\"recreated\":{},\"version\":{},\"bytes\":{},"
                         "\"sampled_copies\":{}}}", t.key, recreate, t.version, bytes, state_->sampled.size());
  return true;
}

// A buffer of the draw layer (vertex or index data, little endian) in device
// memory, created at its first upload and refreshed by ranges copied from the
// batch's upload page, in the open batch before the draws that follow.
bool Backend::UploadBuffer(const BufferUpload& u, std::string& evidence) {
  profile::Timer profile(profile::kEngineBuffer);
  auto fail = [&](const char* reason, VkResult result = VK_ERROR_UNKNOWN) {
    evidence = fmt::format("{{\"uploaded\":false,\"reason\":\"{}\",\"key\":{},\"vk_result\":{}}}", reason, u.key,
                           int32_t(result));
    return false;
  };
  if (!state_->ready || state_->owner_thread != rex::thread::current_thread_system_id() || state_->operation_poisoned ||
      state_->unproved_execution) return fail("native-upload-owner-or-state");
  if (!u.key || !u.bytes || u.ranges.empty() || u.bytes > (uint32_t(64) << 20))
    return fail("native-upload-buffer-description");
  uint64_t range_bytes = 0;
  for (const auto& [offset, data] : u.ranges) {
    if (data.empty() || offset > u.bytes || data.size() > u.bytes - offset) return fail("native-upload-buffer-range");
    range_bytes += data.size();
  }
  VkResult result = VK_SUCCESS;
  const char* batch_reason = nullptr;
  if (!state_->OpenBatch(batch_reason, result)) return fail(batch_reason, result);
  auto& b = state_->buffers[u.key];
  const bool create = !b.buffer || b.bytes != u.bytes;
  if (!create && b.version >= u.version) {
    evidence = fmt::format("{{\"uploaded\":false,\"skipped\":true,\"key\":{},\"version\":{},\"buffer_version\":{}}}",
                           u.key, u.version, b.version);
    return true;
  }
  if (create) {
    if (u.ranges.size() != 1 || u.ranges[0].first || u.ranges[0].second.size() != u.bytes) {
      if (!b.buffer) state_->buffers.erase(u.key);
      return fail("native-upload-buffer-partial-create");
    }
    if (b.buffer) {  // A grown buffer: the old one leaves at once or retired with its sequence.
      if (state_->InFlight(b.last_use)) {
        state_->retired_buffers.push_back(b);
        ++state_->buffers_retired;
      } else {
        ++state_->buffers_released;
      }
      b.buffer.reset();
    }
    result = CreateBuffer(state_->vulkan->allocator, u.bytes,
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0, b.buffer);
    state_->Observe(result);
    if (result != VK_SUCCESS) {
      state_->buffers.erase(u.key);
      return fail("native-upload-buffer-create", result);
    }
    b.bytes = u.bytes;
    b.version = 0;
    b.last_use = 0;
    b.state = {};
    ++state_->buffers_created;
  }
  profile::Count(profile::kUploadBytes, range_bytes);
  profile::Count(profile::kBufferUploadBytes, range_bytes);
  profile::Count(profile::kBufferUploads, 1);
  State::UploadPage* page = nullptr;
  VkDeviceSize page_offset = 0;
  if (!state_->Upload(range_bytes, page, page_offset, result)) return fail("native-upload-buffer-page", result);
  std::vector<VkBufferCopy> copies;
  copies.reserve(u.ranges.size());
  VkDeviceSize at = page_offset;
  for (const auto& [offset, data] : u.ranges) {
    std::memcpy(page->buffer->mapped + at, data.data(), data.size());
    copies.push_back({at, offset, data.size()});
    at += data.size();
  }
  const VkCommandBuffer commands = state_->Commands();
  Transition(commands, b.buffer->buffer, b.state, kBufferCopyDest);
  vkCmdCopyBuffer(commands, page->buffer->buffer, b.buffer->buffer, uint32_t(copies.size()), copies.data());
  Transition(commands, b.buffer->buffer, b.state, kBufferDraw);
  b.version = u.version;
  b.last_use = state_->batch.sequence;
  ++state_->buffer_uploads;
  const uint64_t batch_operations = ++state_->batch.operations;
  ++state_->operations;
  if (batch_operations >= kBatchOperationsMax && !state_->CloseBatch(batch_reason, result))
    return fail(batch_reason, result);
  evidence = fmt::format("{{\"uploaded\":true,\"key\":{},\"created\":{},\"version\":{},\"bytes\":{},\"range_bytes\":{},"
                         "\"ranges\":{},\"buffers\":{}}}", u.key, create, u.version, u.bytes, range_bytes,
                         u.ranges.size(), state_->buffers.size());
  return true;
}

bool Backend::ReleaseBuffer(uint64_t key, std::string& evidence) {
  if (!state_->ready || state_->owner_thread != rex::thread::current_thread_system_id()) {
    evidence = "{\"released\":false,\"reason\":\"native-buffer-release-owner\"}";
    return false;
  }
  auto it = state_->buffers.find(key);
  if (it == state_->buffers.end()) {
    evidence = fmt::format("{{\"released\":true,\"key\":{},\"buffer\":false}}", key);
    return true;
  }
  const uint64_t completed = state_->completion->Completed();
  const uint64_t last_use = it->second.last_use;
  const bool in_flight = state_->InFlight(last_use);
  if (in_flight) {
    state_->retired_buffers.push_back(std::move(it->second));
    ++state_->buffers_retired;
  } else {
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
  auto fail = [&](const char* reason, VkResult result = VK_ERROR_UNKNOWN) {
    evidence = fmt::format("{{\"read\":false,\"reason\":\"{}\",\"key\":{},\"vk_result\":{}}}", reason, key,
                           int32_t(result));
    return false;
  };
  if (!state_->ready || state_->owner_thread != rex::thread::current_thread_system_id() || state_->operation_poisoned ||
      state_->unproved_execution) return fail("native-read-owner-or-state");
  const char* reason = nullptr;
  VkResult result = VK_SUCCESS;
  if (!state_->CloseBatch(reason, result)) return fail(reason, result);
  auto it = state_->textures.find(key);
  if (it == state_->textures.end() || !it->second.texture) return fail("native-read-texture-unknown");
  auto& texture = it->second;
  // A depth destination reads back its R32_FLOAT texels (4 bytes each, like RGBA8).
  if (!texture.content_defined) return fail("native-read-content-undefined");
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {texture.width, texture.height, 1};
  const VkDeviceSize bytes = VkDeviceSize(texture.width) * texture.height * 4;
  uint64_t read_sequence = 0;
  std::shared_ptr<VmaBuffer> buffer;
  if (!state_->ReadImage(texture.Image(), texture.state, bytes, region, read_sequence, buffer, reason, result))
    return fail(reason, result);
  texture.last_use = read_sequence;
  width = texture.width;
  height = texture.height;
  rgba.assign(buffer->mapped, buffer->mapped + bytes);
  evidence = fmt::format("{{\"read\":true,\"key\":{},\"size\":[{},{}],\"format\":\"{}\",\"fence\":{},"
                         "\"operations\":{}}}", key, width, height, texture.depth ? "R32_SFLOAT" : "R8G8B8A8_UNORM",
                         read_sequence, state_->operations);
  return true;
}

bool Backend::ReadSampled(uint32_t key, uint32_t level, std::vector<uint8_t>& rows_out, TextureFormat& format,
                          uint32_t& width, uint32_t& height, uint32_t& row_bytes_out, std::string& evidence) {
  rows_out.clear();
  format = TextureFormat::kUnknown;
  width = height = row_bytes_out = 0;
  auto fail = [&](const char* reason, VkResult result = VK_ERROR_UNKNOWN) {
    evidence = fmt::format("{{\"read\":false,\"reason\":\"{}\",\"key\":{},\"level\":{},\"vk_result\":{}}}", reason,
                           key, level, int32_t(result));
    return false;
  };
  if (!state_->ready || state_->owner_thread != rex::thread::current_thread_system_id() || state_->operation_poisoned ||
      state_->unproved_execution) return fail("native-read-owner-or-state");
  const char* reason = nullptr;
  VkResult result = VK_SUCCESS;
  if (!state_->CloseBatch(reason, result)) return fail(reason, result);
  auto it = state_->sampled.find(key);
  if (it == state_->sampled.end() || !it->second.image) return fail("native-read-sampled-unknown");
  auto& copy = it->second;
  if (level >= copy.levels) return fail("native-read-sampled-level");
  // Face 0 of the level, its rows packed as the uploads carry them.
  const LevelRows rows = RowsOfLevel(copy.format, copy.width, copy.height, level);
  const VkDeviceSize bytes = VkDeviceSize(rows.rows) * rows.row_bytes;
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
  region.imageExtent = {std::max(copy.width >> level, 1u), std::max(copy.height >> level, 1u), 1};
  uint64_t read_sequence = 0;
  std::shared_ptr<VmaBuffer> buffer;
  if (!state_->ReadImage(copy.image->image, copy.state, bytes, region, read_sequence, buffer, reason, result))
    return fail(reason, result);
  copy.last_use = read_sequence;
  rows_out.assign(buffer->mapped, buffer->mapped + bytes);
  format = copy.format;
  width = region.imageExtent.width;
  height = region.imageExtent.height;
  row_bytes_out = rows.row_bytes;
  evidence = fmt::format("{{\"read\":true,\"key\":{},\"level\":{},\"format\":{},\"size\":[{},{}],\"rows\":{},"
                         "\"row_bytes\":{}}}", key, level, uint32_t(format), width, height, rows.rows, rows.row_bytes);
  return true;
}

bool Backend::ReadSurface(uint32_t key, uint64_t after_fence, std::vector<uint8_t>& rgba, uint32_t& width,
                          uint32_t& height, std::string& evidence) {
  rgba.clear();
  width = height = 0;
  auto fail = [&](const char* reason, VkResult result = VK_ERROR_UNKNOWN) {
    evidence = fmt::format("{{\"read\":false,\"reason\":\"{}\",\"key\":{},\"after_fence\":{},\"vk_result\":{}}}",
                           reason, key, after_fence, int32_t(result));
    return false;
  };
  if (!state_->ready || state_->owner_thread != rex::thread::current_thread_system_id() || state_->operation_poisoned ||
      state_->unproved_execution) return fail("native-read-surface-owner-or-state");
  const char* reason = nullptr;
  VkResult result = VK_SUCCESS;
  if (!state_->CloseBatch(reason, result)) return fail(reason, result);
  auto it = state_->surfaces.find(key);
  if (it == state_->surfaces.end() || !it->second.texture) return fail("native-read-surface-unknown");
  auto& surface = it->second;
  if (surface.desc.format != SurfaceFormat::Color8888 || surface.desc.samples != 1)
    return fail("native-read-surface-format");
  // The read follows the operation of after_fence, and nothing else has used
  // the surface since.
  if (!after_fence || after_fence >= state_->next_sequence || surface.last_use != after_fence)
    return fail("native-read-surface-fence");
  if (!state_->completion->WaitFor(after_fence, kWaitBudget)) return fail("native-read-surface-wait", VK_TIMEOUT);
  const uint64_t completed_before = state_->completion->Completed();
  const char* layout_before = LayoutName(surface.state.layout);
  const uint32_t w = surface.desc.width, h = surface.desc.height;
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {w, h, 1};
  uint64_t read_sequence = 0;
  std::shared_ptr<VmaBuffer> buffer;
  if (!state_->ReadImage(surface.Image(), surface.state, VkDeviceSize(w) * h * 4, region, read_sequence, buffer,
                         reason, result)) return fail(reason, result);
  surface.last_use = read_sequence;
  width = w;
  height = h;
  rgba.assign(buffer->mapped, buffer->mapped + size_t(w) * h * 4);
  evidence = fmt::format("{{\"read\":true,\"key\":{},\"size\":[{},{}],\"format\":\"R8G8B8A8_UNORM\",\"samples\":{},"
                         "\"layout_before\":\"{}\",\"after_fence\":{},\"completed_before_copy\":{},\"read_fence\":{},"
                         "\"row_pitch\":{},\"operations\":{}}}", key, w, h, surface.desc.samples, layout_before,
                         after_fence, completed_before, read_sequence, w * 4, state_->operations);
  return true;
}

uint32_t Backend::AllocateView() {
  if (state_->free_views.empty()) return state_->next_view++;
  const uint32_t view = state_->free_views.back();
  state_->free_views.pop_back();
  return view;
}

#if defined(NATIVE_RENDERER_ENGINE_FAULT_TEST)
bool Backend::SeedSampledForTest(uint32_t address, bool in_flight, std::string& evidence) {
  if (!state_->ready || state_->owner_thread != rex::thread::current_thread_system_id() || state_->sampled.count(address)) return false;
  VkResult result = VK_SUCCESS;
  const char* reason = nullptr;
  if (in_flight && !state_->OpenBatch(reason, result)) return false;
  auto& s = state_->sampled[address];
  if (CreateImage(state_->vulkan->allocator, VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, false, s.image) != VK_SUCCESS) {
    state_->sampled.erase(address);
    return false;
  }
  s.format = TextureFormat::kR8G8B8A8Unorm;
  s.width = s.height = 4;
  s.levels = 1;
  s.view = AllocateView();
  s.last_use = in_flight ? state_->batch.sequence : 0;
  evidence = fmt::format("{{\"seeded\":true,\"address\":{},\"view\":{},\"last_use_fence\":{},\"counts\":{}}}",
                         address, s.view, s.last_use, ResourceCounts());
  return true;
}

CompletionTestHooks* Backend::CompletionHooksForTest() {
  if (!state_->completion) return nullptr;
  if (!state_->hooks) state_->hooks = std::make_unique<State::Hooks>(*state_);
  return state_->hooks.get();
}

bool Backend::ReadBufferForTest(uint64_t key, std::vector<uint8_t>& bytes, std::string& evidence) {
  bytes.clear();
  auto fail = [&](const char* reason, VkResult result = VK_ERROR_UNKNOWN) {
    evidence = fmt::format("{{\"read\":false,\"reason\":\"{}\",\"key\":{},\"vk_result\":{}}}", reason, key,
                           int32_t(result));
    return false;
  };
  if (!state_->ready || state_->owner_thread != rex::thread::current_thread_system_id()) return fail("native-read-owner-or-state");
  const char* reason = nullptr;
  VkResult result = VK_SUCCESS;
  if (!state_->CloseBatch(reason, result)) return fail(reason, result);
  auto it = state_->buffers.find(key);
  if (it == state_->buffers.end()) return fail("native-read-buffer-unknown");
  auto& b = it->second;
  uint64_t read_sequence = 0;
  std::shared_ptr<VmaBuffer> buffer;
  if (!state_->ReadBack(b.bytes, [&](VkCommandBuffer commands, VkBuffer target) {
        Transition(commands, b.buffer->buffer, b.state, kBufferCopySource);
        const VkBufferCopy copy{0, 0, b.bytes};
        vkCmdCopyBuffer(commands, b.buffer->buffer, target, 1, &copy);
      }, read_sequence, buffer, reason, result)) return fail(reason, result);
  b.last_use = read_sequence;
  bytes.assign(buffer->mapped, buffer->mapped + b.bytes);
  evidence = fmt::format("{{\"read\":true,\"key\":{},\"bytes\":{}}}", key, b.bytes);
  return true;
}
#endif
}  // namespace gpu::vulkan

namespace gpu {
std::unique_ptr<Backend> CreateVulkanBackend(std::shared_ptr<FailureChannel> failures) {
  return std::make_unique<vulkan::Backend>(std::move(failures));
}
}  // namespace gpu
