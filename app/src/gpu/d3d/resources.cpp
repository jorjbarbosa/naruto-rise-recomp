// Guest resources of the D3D boundary (d3d/resources.h): creation
// of surfaces and textures, their destruction (D750), the texture locks with
// the write back of an own resolve, and the last Unlock.
#include "gpu/d3d/resources.h"
#include "gpu/diagnostics/diagnostics.h"
#include "gpu/diagnostics/heap_watch.h"
#include "gpu/d3d/hooks.h"
#include "gpu/d3d/device.h"
#include "gpu/d3d/draw.h"
#include "gpu/shaders/shaders.h"
#include "gpu/textures/textures.h"
#include "gpu/buffers/buffers.h"
#include "gpu/settings.h"
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
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>
#include <rex/system/xmemory.h>

using namespace gpu;

namespace gpu {
ResourceRegistry resources;
std::atomic<uint32_t> resource_records{0};
bool RecordResource() { return Diagnostics() && resource_records.fetch_add(1) < 24; }
TextureContent texture_content;
std::string ResourceSummary() { return resources.Summary() + "," + texture_content.Summary(); }
}  // namespace gpu

// --- Resources ---
namespace gpu {
// The color formats an own RGBA8 surface covers, from the D3DFORMAT word
// (+0x28) and RB_COLOR_INFO (+0x1C) of the header: texture format 8_8_8_8
// without gamma, signed or biased components (sign fields, bits 9 to 16) and
// with fraction numbers (bit 17); color_format 8_8_8_8 (not 8_8_8_8_GAMMA) and
// exponent bias 0. Endian, tiling and swizzle only describe the CPU and texture
// views of resolved data, which the destination's fetch constant governs.
// Returns the first field outside, or null.
const char* UncoveredColorField(uint32_t format, uint32_t info) {
  if ((format & 0x3F) != 6) return "D3DFORMAT.texture_format";
  for (unsigned c = 0; c < 4; ++c)
    if (((format >> (9 + 2 * c)) & 3) == 3) return "D3DFORMAT.sign_gamma";
  if ((format >> 9) & 0xFF) return "D3DFORMAT.sign";
  if ((format >> 17) & 1) return "D3DFORMAT.num_format";
  const uint32_t color_format = (info >> 16) & 0xF;
  if (color_format == 1) return "RB_COLOR_INFO.color_format_gamma";
  if (color_format) return "RB_COLOR_INFO.color_format";
  if ((info >> 20) & 0x3F) return "RB_COLOR_INFO.color_exp_bias";
  return nullptr;
}
bool DepthFormatCovered(uint32_t format) { return (format & 0x3F) == 22 || (format & 0x3F) == 23; }
}  // namespace gpu
// CreateRenderTarget / CreateDepthStencilSurface: (W, H, Format, MultiSample,
// pParameters) -> 48-byte surface header. The original builds the header; the
// own surface uses SizeBits (+0x24) and Format (+0x28) of that header.
extern "C" void __imp__sub_8212D330(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_8212D330) {
  if (!gpu::Active()) { __imp__sub_8212D330(ctx, base); return; }
  HookScope scope("D330-CreateSurface");
  const std::array<uint32_t, 8> args{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                                     ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
  const uint32_t lr = ctx.lr;
  __imp__sub_8212D330(ctx, base);
  gpu::profile::Timer hook_time(gpu::profile::kHookCreate);
  const uint32_t header = ctx.r3.u32;
  if (!header) return;  // Original allocation failure is the title's to handle.
  auto* ts = rex::runtime::ThreadState::Get();
  std::array<uint8_t, 48> bytes{};
  if (!ts || !ReadVirtual(ts->memory(), header, bytes.data(), bytes.size()))
    StopInOrder(ctx, "D330-header-unreadable", 88);
  const uint32_t size_bits = BigEndianWord(bytes.data() + 0x24), format = BigEndianWord(bytes.data() + 0x28);
  SurfaceDesc desc{header, (size_bits >> 18) + 1, ((size_bits >> 3) & 0x7FFF) + 1, format};
  const bool depth = DepthFormatCovered(format);
  // The full color format (gamma included), not only its 6 low bits.
  const char* uncovered = depth ? nullptr : UncoveredColorField(format, BigEndianWord(bytes.data() + 0x1C));
  const bool supported = !uncovered;
  desc.format = depth ? SurfaceFormat::DepthStencil : SurfaceFormat::Color8888;
  // D3DMULTISAMPLE_TYPE of the Xbox 360: 1 = 2 samples, 2 = 4 samples.
  desc.samples = args[3] == 2 ? 4 : args[3] == 1 ? 2 : 1;
  if (desc.width != args[0] || desc.height != args[1]) StopInOrder(ctx, "D330-header-size-mismatch", 88);
  std::string evidence = fmt::format("{{\"created\":false,\"reason\":\"guest-format-not-mapped\",\"field\":\"{}\"}}",
                                     uncovered ? uncovered : "");
  if (supported) {
    auto* dispatcher = gpu::host::ActiveDispatcher();
    if (!dispatcher || !dispatcher->Run([desc](Engine& engine, std::string& out) {
          return engine.CreateSurface(desc, out); }, evidence))
      StopInOrder(ctx, "D330-own-surface-unproved", 88);
  }
  const uint64_t generation = resources.Register(header, ResourceRegistry::Kind::Surface, supported, args);
  if (!generation) StopInOrder(ctx, "D330-header-key-live", 88);
  if (RecordResource())
    gpu::host::Record("native-d3d-surface", fmt::format(
        "\"header\":{},\"generation\":{},\"width\":{},\"height\":{},\"format\":{},\"lr\":{},"
        "\"guest_thread\":{},\"multisample\":{},\"parameters\":{},\"engine\":{}", header, generation,
        desc.width, desc.height, format, lr, ts->thread_id(), args[3], args[4], evidence));
}
// CreateTexture: descriptor and generation; the texture layer registers
// the header by its own fetch constant, and uploads it at its last Unlock or
// first binding.
extern "C" void __imp__sub_8212D210(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_8212D210) {
  if (!gpu::Active()) { __imp__sub_8212D210(ctx, base); return; }
  HookScope scope("D210-CreateTexture");
  const std::array<uint32_t, 8> args{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                                     ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
  const uint32_t lr = ctx.lr;
  __imp__sub_8212D210(ctx, base);
  gpu::profile::Timer hook_time(gpu::profile::kHookCreate);
  const uint32_t header = ctx.r3.u32;
  if (!header) return;
  const uint64_t generation = resources.Register(header, ResourceRegistry::Kind::Texture, false, args);
  if (!generation) StopInOrder(ctx, "D210-header-key-live", 88);
  RegisterTextureHeader(base, header);
  if (RecordResource()) {
    auto* ts = rex::runtime::ThreadState::Get();
    std::array<uint8_t, 52> bytes{};
    if (!ts || !ReadVirtual(ts->memory(), header, bytes.data(), bytes.size()))
      StopInOrder(ctx, "D210-header-unreadable", 88);
    std::string hex;
    for (uint8_t b : bytes) hex += fmt::format("{:02x}", b);
    gpu::host::Record("native-d3d-texture", fmt::format(
        "\"header\":{},\"generation\":{},\"args\":[{}],\"header_hex\":\"{}\",\"lr\":{},\"guest_thread\":{}",
        header, generation, fmt::join(args, ","), hex, lr, ts->thread_id()));
  }
}
namespace gpu {
// The own resource of a registered guest header, released on the engine owner:
// its surface, or the own texture of a resolve destination, freed once
// its last use completed (Engine::ReleaseTexture). The registry
// follows the engine: the entry is removed only after a confirmed release.
// Shared by D750 and the ordered close. False: unproved (evidence says why).
std::atomic<uint32_t> d750_texture_records{0}, d750_shader_records{0};
bool ReleaseRegistered(uint32_t key, std::string& evidence) {
  ResourceRegistry::Entry entry{};
  auto* dispatcher = host::ActiveDispatcher();
  if (!resources.Find(key, entry)) {
    // In order with the captures (the registered path forgets after its Run, which is).
    if (dispatcher) dispatcher->Defer([key] { texture_content.Forget(key); });
    else texture_content.Forget(key);
    evidence = "{\"registered\":false}";
    return true;
  }
  const bool surface = entry.host_surface;
  if (!dispatcher || !dispatcher->Run([key, surface](Engine& engine, std::string& out) {
        return surface ? engine.DestroySurface(key, out) : engine.ReleaseTexture(key, out); }, evidence))
    return false;
  if (!resources.Take(key, entry.generation, entry)) {
    evidence = "{\"registry\":\"generation-changed\"}";
    return false;
  }
  texture_content.Forget(key);
  return true;
}
// The sampled copy of a texture header's memory does not outlive the
// header. Copies are keyed by the physical base of the fetch constant;
// the header holds the guest virtual base (+0x20), converted as the D3D does
// (GuestPhysicalAlias). The texture layer forgets the base and the engine
// releases a posted copy, at once or retired with the fence of its last use.
// Other resource types (Common low bits, D3DRESOURCETYPE 3 = texture) cost nothing.
std::atomic<uint32_t> d750_sampled_records{0}, d750_buffer_records{0};
bool ReleaseSampledCopy(uint8_t* base, uint32_t header, std::string& evidence) {
  if ((Get32(base, header) & 0xF) != 3) return true;
  return ReleaseTextureBase(GuestPhysicalAlias(Get32(base, header + 0x20) & 0xFFFFF000u), evidence);
}
}  // namespace gpu
// Resource destruction (reached from D3DResource_Release at refcount zero):
// the own resource goes first, then the original frees the guest header.
extern "C" void __imp__sub_8212D750(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_8212D750) {
  if (gpu::Active()) {
    HookScope scope("D750-destroy");
    gpu::profile::Timer hook_time(gpu::profile::kHookDestroy);
    const uint32_t key = ctx.r3.u32;
    ResourceRegistry::Entry entry{};
    const bool registered = resources.Find(key, entry);
    const bool resolved = texture_content.Resolved(key);
    std::string evidence;
    if (!ReleaseRegistered(key, evidence))
      StopInOrder(ctx, registered && entry.host_surface ? "D750-own-surface-release-unproved" :
                    "D750-own-texture-release-unproved", 88);
    if (registered && !entry.host_surface && resolved && Diagnostics() && d750_texture_records.fetch_add(1) < 16)
      gpu::host::Record("native-d3d-d750-texture", fmt::format(
          "\"header\":{},\"generation\":{},\"lr\":{},\"guest_thread\":{},\"engine\":{}", key, entry.generation,
          ctx.lr, rex::runtime::ThreadState::Get() ? rex::runtime::ThreadState::Get()->thread_id() : 0, evidence));
    std::string sampled;
    if (!ReleaseSampledCopy(base, key, sampled)) StopInOrder(ctx, "D750-sampled-copy-release-unproved", 88);
    if (!sampled.empty() && Diagnostics() && d750_sampled_records.fetch_add(1) < 16)
      gpu::host::Record("native-d3d-d750-sampled", fmt::format(
          "\"header\":{},\"registered\":{},\"lr\":{},\"guest_thread\":{},\"engine\":{}", key, registered, ctx.lr,
          rex::runtime::ThreadState::Get() ? rex::runtime::ThreadState::Get()->thread_id() : 0, sampled));
    // A destroyed shader object leaves the registry.
    if (ForgetShaderObject(key) && Diagnostics() && d750_shader_records.fetch_add(1) < 16)
      gpu::host::Record("native-d3d-d750-shader", fmt::format("\"object\":{},\"lr\":{}", key, ctx.lr));
    // A destroyed vertex or index buffer header retires the buffer entries at its base.
    if (const size_t entries = ForgetBufferHeader(base, key); entries && Diagnostics() && d750_buffer_records.fetch_add(1) < 16)
      gpu::host::Record("native-d3d-d750-buffer", fmt::format("\"header\":{},\"entries\":{},\"lr\":{}",
                                                                  key, entries, ctx.lr));
  }
  __imp__sub_8212D750(ctx, base);
}

// --- Texture locks ---
// 8212C7E8 computes the level and face layout on the CPU (8212C378, 8212B570)
// and locks the texture data through 8212D950 with reason 14, whose own path
// accumulates the range instead of emitting. D3DTexture_LockRect (D030),
// D3DCubeTexture_LockRect (D118) and D3DSurface_LockRect (D530) reach it through
// 8212C8B0. The title then reads and writes guest memory directly. A texture
// whose content is an own resolve has no current guest copy, so its lock stays
// a guard; every other lock counts a CPU write of the header.
namespace gpu {
std::atomic<uint32_t> texture_lock_records{0};
std::atomic<uint32_t> resolved_lock_records{0};
// A resolve destination the CPU locks (the D3DX-like clone 822E00A8 of
// the fight's freeze frame, a 320x180 capture) gets the own resolve written
// to its guest memory first, as the console's resolve leaves it there: read
// back from the GPU (ReadTexture, which waits for the queue) and stored in the
// layout of the header's fetch constant, the inverse of ReadTextureLevels.
// k_5_6_5 is covered: D3DFMT_R5G6B5, red in bits 11..15, which the fetch
// swizzle ZYX1 reads back as the own RGBA; another format keeps the guest
// memory as it was. With an
// internal resolution scale the own texture is larger than the header; each
// guest texel gets the area average of the host texels it covers.
bool WriteBackResolve(uint8_t* base, uint32_t header, std::string& evidence) {
  std::array<uint32_t, 6> d;
  for (unsigned i = 0; i < 6; ++i) d[i] = Get32(base, header + 0x1C + 4 * i);
  d = PhysicalFetch(d);
  const uint32_t format = d[1] & 0x3F, endian = (d[1] >> 6) & 3, address = d[1] & 0xFFFFF000u;
  const uint32_t width = (d[2] & 0x1FFF) + 1, height = ((d[2] >> 13) & 0x1FFF) + 1;
  const uint32_t pitch = ((d[0] >> 22) & 0x1FF) * 32;
  const bool tiled = d[0] >> 31;
  auto fail = [&](const char* reason, const std::string& detail = "null") {
    evidence = fmt::format("{{\"written\":false,\"reason\":\"{}\",\"format\":{},\"size\":[{},{}],\"pitch\":{},"
                           "\"tiled\":{},\"detail\":{}}}", reason, format, width, height, pitch, tiled, detail);
    return false;
  };
  if (format != 4) return fail("format-uncovered");
  if (!address || pitch < width) return fail("layout-uncovered");
  std::vector<uint8_t> rgba;
  uint32_t read_width = 0, read_height = 0;
  std::string read;
  auto* dispatcher = host::ActiveDispatcher();
  if (!dispatcher || !dispatcher->Run([&](Engine& engine, std::string& out) {
        return engine.ReadTexture(header, rgba, read_width, read_height, out); }, read))
    return fail("readback", read.empty() ? "null" : read);
  const ResolutionScale& scale = CurrentSettings().resolution_scale;
  if (read_width != scale.Width(width) || read_height != scale.Height(height) ||
      rgba.size() < size_t(read_width) * read_height * 4)
    return fail("size-mismatch", fmt::format("[{},{}]", read_width, read_height));
  if (!scale.Unit()) rgba = DownscaleRgba(rgba, read_width, read_height, width, height);
  constexpr uint32_t bpb = 2, log2_bpp = 1;
  const size_t region = size_t(pitch) * ((height + 31) & ~31u) * bpb;
  uint8_t* const physical = base + 0x100000000ull + address;
  std::vector<uint8_t> texel(bpb);
  for (uint32_t y = 0; y < height; ++y) {
    const uint32_t outer = TiledOffset2DOuter(y, pitch, log2_bpp);
    for (uint32_t x = 0; x < width; ++x) {
      const uint32_t at = tiled ? (TiledOffset2DInner(x, y, log2_bpp, outer) >> log2_bpp) << log2_bpp
                                : (y * pitch + x) * bpb;
      if (at + bpb > region) return fail("tile-range");
      const uint8_t* p = &rgba[(size_t(y) * width + x) * 4];
      const uint32_t word = ((p[0] * 31u + 127) / 255) << 11 | ((p[1] * 63u + 127) / 255) << 5 |
                            ((p[2] * 31u + 127) / 255);
      texel[0] = uint8_t(word);
      texel[1] = uint8_t(word >> 8);
      SwapEndian(texel, endian);
      std::memcpy(physical + at, texel.data(), bpb);
    }
  }
  evidence = fmt::format("{{\"written\":true,\"format\":{},\"size\":[{},{}],\"read_size\":[{},{}],\"pitch\":{},"
                         "\"tiled\":{},\"endian\":{},\"physical\":{},\"bytes\":{}}}", format, width, height,
                         read_width, read_height, pitch, tiled, endian, address, region);
  return true;
}
}  // namespace gpu
extern "C" void __imp__sub_8212C7E8(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_8212C7E8) {
  if (!gpu::Active()) { __imp__sub_8212C7E8(ctx, base); return; }
  HookScope scope("C7E8-TextureLock");
  if (!device_owner || !device_owner->live()) StopInOrder(ctx, "C7E8-without-device", 88);
  const uint32_t texture = ctx.r3.u32;
  if (!texture) StopInOrder(ctx, "C7E8-null-texture", 88);
  const std::array<uint32_t, 8> args{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                                     ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
  const uint32_t lr = ctx.lr;
  if (texture_content.Resolved(texture)) {
    // 822E00A8, a D3DX-like texture clone (it locks both textures and
    // copies or converts on the CPU), reads a resolve destination: the own
    // resolve is written back to its guest memory first (WriteBackResolve);
    // the texture stays a resolve (no CPU write noted).
    std::string writeback;
    gpu::heap_watch::NoteResolvedLock(true, texture);  // The save crash (heap_watch.h).
    WriteBackResolve(base, texture, writeback);
    __imp__sub_8212C7E8(ctx, base);
    gpu::heap_watch::NoteResolvedLock(false, texture);
    if (Diagnostics() && resolved_lock_records.fetch_add(1) < 8)
      gpu::host::Record("native-d3d-texture-lock-resolved", fmt::format(
          "\"texture\":{},\"args\":[{}],\"lr\":{},\"guest_thread\":{},\"fetch\":[{},{},{},{},{},{}],\"writeback\":{}",
          texture, fmt::join(args, ","), lr, GuestThread(), Get32(base, texture + 0x1C), Get32(base, texture + 0x20),
          Get32(base, texture + 0x24), Get32(base, texture + 0x28), Get32(base, texture + 0x2C),
          Get32(base, texture + 0x30), writeback));
    return;
  }
  __imp__sub_8212C7E8(ctx, base);  // Layout and the own D950 path.
  gpu::profile::Timer hook_time(gpu::profile::kHookLock);  // D950 times itself.
  const uint64_t writes = texture_content.NoteCpuWrite(texture);
  if (Diagnostics() && texture_lock_records.fetch_add(1) < 16) {
    ResourceRegistry::Entry entry{};
    const bool registered = resources.Find(texture, entry);
    // r7 receives the data pointer, r8 the pitch and r10 the offset (stores at
    // the end of 8212C7E8).
    gpu::host::Record("native-d3d-texture-lock", fmt::format(
        "\"texture\":{},\"args\":[{}],\"lr\":{},\"guest_thread\":{},\"data\":{},\"pitch\":{},\"offset\":{},"
        "\"fetch\":[{},{},{},{},{},{}],\"registered\":{},\"generation\":{},\"cpu_writes\":{}", texture,
        fmt::join(args, ","), lr, GuestThread(), Get32(base, args[4]), Get32(base, args[5]), Get32(base, args[7]),
        Get32(base, texture + 0x1C), Get32(base, texture + 0x20), Get32(base, texture + 0x24),
        Get32(base, texture + 0x28), Get32(base, texture + 0x2C), Get32(base, texture + 0x30), registered,
        registered ? entry.generation : 0, writes));
  }
}

// --- Unlock ---
// 8212DBE0(resource, base, mip address) unlocks textures, vertex and index
// buffers (IDA): an atomic decrement of the lock count (Common bits 8..11); the
// last one flushes the CPU-written ranges kept at +0x14 (base) and +0x18
// (mips) and resets them. The texture layer takes those ranges as a write of
// their pages and uploads a texture (NoteUnlock).
extern "C" void __imp__sub_8212DBE0(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_8212DBE0) {
  if (!gpu::Active()) { __imp__sub_8212DBE0(ctx, base); return; }
  HookScope scope("DBE0-Unlock");
  const uint32_t resource = ctx.r3.u32, base_address = ctx.r4.u32, mip_address = ctx.r5.u32;
  const uint32_t common = Get32(base, resource), lines = Get32(base, resource + 0x14);
  const uint32_t mip_lines = Get32(base, resource + 0x18);
  PPCContext entry = ctx;
  __imp__sub_8212DBE0(ctx, base);
  if ((common & 0xF00) == 0x100) {  // The last Unlock.
    gpu::profile::Timer hook_time(gpu::profile::kHookUnlock);
    NoteUnlock(entry, base, resource, common, base_address, lines, mip_address, mip_lines);
  }
}
