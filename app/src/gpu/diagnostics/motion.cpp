// The motion record of the frames the title builds (motion.h).
#include "gpu/common/platform.h"
#include "diagnostics/report_context.h"
#include "gpu/d3d/destruction_order.h"
#include "gpu/diagnostics/motion.h"
#include "gpu/diagnostics/diagnostics.h"
#include "gpu/diagnostics/sampler.h"
#include "gpu/interpolation/tick.h"
#include "gpu/interpolation/transforms.h"
#include "gpu/common/guest_memory.h"
#include "gpu/host/services.h"
#include "gpu/textures/formats.h"
#include <fmt/format.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

extern "C" void __imp__sub_8265A058(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8267A2A0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_820FC008(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_820E91E0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_826AB438(PPCContext& __restrict, uint8_t*);

namespace gpu::motion {
namespace {
constexpr uint32_t kCutCounter = 0x82C0EB34;
constexpr uint32_t kDisplayData = 0x82C0EFA4;    // +228: the object the display draws.
constexpr uint32_t kPoseCopyReturn = 0x82580BF4;  // The bone copy's call in 82580968.
constexpr uint32_t kBoneStride = 6;              // Every 6th bone of a palette goes to the record.
enum Stage { kLoop, kDisplayIn, kDisplayOut, kDelivered, kListsIn, kListsOut, kStages };
constexpr const char* kStageNames[kStages] = {"loop", "display_in", "display_out", "delivered", "lists_in",
                                              "lists_out"};
constexpr size_t kRangesMax = 256;
constexpr uint32_t kRangeBytesMax = 64 * 1024;

int enabled = -1;
float probe_units = 0.0f;
std::FILE* file = nullptr;
int64_t frequency = 0, start = 0;
// Main thread.
uint64_t displays = 0, cameras = 0, main_steps = 0, mounts = 0;
uint64_t last_displays = 0, last_cameras = 0, last_main_steps = 0;
int64_t last_record = 0;
bool in_display = false;
uint32_t mounted = 0, mounted_outside = 0;
std::unordered_set<uint32_t> live_set;
// The objects with dynamics of this display and their positions.
struct Point { uint32_t object; float x, y, z; };
std::vector<Point> moving_points;
// The bones copied into the frame's packet (820E91E0): inside the display,
// outside it, from another call than the pose copy's, of an object with
// dynamics, of a skinned object near one (kOwnerDistance), far from any; and
// the record's bones of this display.
constexpr float kOwnerDistance = 3.0f;
uint64_t bone_copies = 0, bone_copies_outside = 0, bone_copies_other = 0;
uint64_t bone_copies_moving = 0, bone_copies_near = 0, bone_copies_far = 0;
uint32_t display_thread = 0;  // The guest thread of the display.
struct BoneRow { uint32_t owner, object, bone; float x, y, z, a, b, c, distance; };
std::vector<BoneRow> bone_rows;
// The interpolation of the poses (transforms.h), summed over the displays.
uint64_t poses = 0, bones_written = 0, bone_cuts = 0, bones_lost = 0, pose_other_thread = 0;
// The effects' update on the work thread (826AB438, r3 the list: +0 the
// effects, +4 their count), posted by the logic in the tick (82650610) and
// waited for by the effects' draw (826AC2E8) inside the view block: the
// updates still running once the view block of an iteration with a tick has
// started (the block cannot end before them), how long they ran into it, and
// the effects' update functions (their vtable's +12).
std::atomic<int64_t> view_tick_start{0};  // QPC of the view block of a tick, 0 outside it.
std::mutex effects_mutex;
std::map<uint32_t, uint64_t> effect_updates;  // Update function -> effects seen.
uint64_t effect_runs = 0, effect_overlaps = 0, view_ticks = 0;
// The view block's waits for that update (transforms.h), main thread.
uint64_t effect_waits = 0, effect_wait_us_sum = 0, effect_wait_us_max = 0, effect_wait_timeouts = 0;
double effect_ms_sum = 0, effect_ms_max = 0, overlap_ms_sum = 0, overlap_ms_max = 0;
int64_t Qpc() { return platform::Ticks(); }
double QpcMs(int64_t ticks) {
  static const int64_t f = platform::TickFrequency();
  return double(ticks) * 1000.0 / double(f);
}
void EffectsUpdate(PPCContext& ctx, uint8_t* base) {
  const int64_t start = Qpc();
  const uint32_t list = ctx.r3.u32, array = Get32(base, list), count = Get32(base, list + 4);
  std::map<uint32_t, uint64_t> seen;
  for (uint32_t i = 0; array && i < count && i < 4096; ++i) {
    const uint32_t effect = Get32(base, array + 4 * i);
    const uint32_t table = effect ? Get32(base, effect) : 0;
    if (table) ++seen[Get32(base, table + 12)];
  }
  __imp__sub_826AB438(ctx, base);
  const int64_t end = Qpc(), view = view_tick_start.load();
  std::lock_guard lock(effects_mutex);
  ++effect_runs;
  const double ms = QpcMs(end - start);
  effect_ms_sum += ms;
  effect_ms_max = std::max(effect_ms_max, ms);
  if (view) {
    ++effect_overlaps;
    const double into = QpcMs(end - std::max(view, start));
    overlap_ms_sum += into;
    overlap_ms_max = std::max(overlap_ms_max, into);
  }
  for (const auto& [fn, n] : seen)
    if (effect_updates.size() < 64 || effect_updates.count(fn)) effect_updates[fn] += n;
}
// The vertex textures, by physical address (translation, main and render threads).
struct Range {
  uint32_t bytes = 0, format = 0;
  uint64_t hash = 0, seen = 0;
  int stage = -1;
};
std::mutex ranges_mutex;
std::map<uint32_t, Range> ranges;
uint64_t notes = 0, distinct = 0, hashed_ranges = 0;
uint64_t changes[2][kStages][kStages]{};  // [tick][from][to]
std::atomic<uint64_t> loop_index{0};
std::atomic<bool> iteration_tick{true};

uint64_t Hash(const uint8_t* data, size_t size) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < size; ++i) h = (h ^ data[i]) * 0x100000001B3ull;
  return h;
}

void StageHash(uint8_t* base, Stage stage) {
  std::vector<uint8_t> buffer;
  std::lock_guard lock(ranges_mutex);
  const int tick = iteration_tick.load() ? 1 : 0;
  for (auto& [address, r] : ranges) {
    buffer.resize(r.bytes);
    if (!ReadPhysical(base, address, r.bytes, buffer.data())) continue;
    const uint64_t h = Hash(buffer.data(), buffer.size());
    if (r.stage >= 0 && h != r.hash) ++changes[tick][r.stage][stage];
    r.hash = h;
    r.stage = stage;
    ++hashed_ranges;
  }
}

// The live list of a world and its objects with dynamics.
std::vector<uint32_t> LiveObjects(uint8_t* base, uint32_t world) {
  std::vector<uint32_t> out;
  if (!world) return out;
  const uint32_t first = Get32(base, world + 240), last = Get32(base, world + 228) - 8;
  if (!first || last < first || last - first > 8 * 65536) return out;
  for (uint32_t entry = first; entry <= last; entry += 8) {
    const uint32_t object = Get32(base, entry);
    if (object && (object & 3) != 1) out.push_back(object);
  }
  return out;
}
bool HasDynamics(uint8_t* base, uint32_t object) {
  if (!(Get32(base, object + 4) & 0x00040000)) return false;
  const uint32_t extension = Get32(base, object + 28);
  const uint32_t dynamics = extension ? Get32(base, extension + 4) : 0;
  return dynamics && (Get32(base, dynamics) & 0x80000000);
}
float Finite(float f) { return std::isfinite(f) ? f : 0.0f; }
// A point through a matrix of the engine (row vectors; with the flag 8 at +64,
// the rows scaled by +12, +28 and +44).
void Through(uint8_t* base, uint32_t m, const float* in, float* out) {
  const bool scaled = (Get32(base, m + 64) & 8) != 0;
  for (uint32_t c = 0; c < 3; ++c) {
    float v = GetFloat(base, m + 48 + 4 * c);
    for (uint32_t r = 0; r < 3; ++r)
      v += in[r] * GetFloat(base, m + 16 * r + 4 * c) * (scaled ? GetFloat(base, m + 16 * r + 12) : 1.0f);
    out[c] = v;
  }
}

// A bone copied into the frame's packet (820E91E0, r3 the bone, r4 its matrix
// relative to the skinned object, the pose copy's (transforms.h) or [display
// state + 228]): for every kBoneStride-th bone of a skinned object within
// kOwnerDistance of an object with dynamics (the owner, the nearest), the
// world point of the bone (its translation through the skinned object's
// matrix at the copy), that translation and the distance to the owner.
void BoneCopy(uint8_t* base, uint32_t bone, uint32_t matrix, uint32_t return_address) {
  if (!in_display || GuestThread() != display_thread) {
    ++bone_copies_outside;
    return;
  }
  ++bone_copies;
  if (return_address != kPoseCopyReturn) ++bone_copies_other;
  const uint32_t display = Get32(base, kDisplayData);
  uint32_t object = interpolation::PoseObject();
  if (!object && display) object = Get32(base, display + 228);
  const uint32_t m = object ? Get32(base, object + 20) : 0;
  if (!m) return;
  const float at[3] = {GetFloat(base, m + 48), GetFloat(base, m + 52), GetFloat(base, m + 56)};
  uint32_t owner = 0;
  float distance = kOwnerDistance;
  for (const auto& p : moving_points) {
    const float d = p.object == object ? 0.0f : std::hypot(at[0] - p.x, at[1] - p.y, at[2] - p.z);
    if (d <= distance) {
      distance = d;
      owner = p.object;
    }
  }
  if (!owner) {
    ++bone_copies_far;
    return;
  }
  ++(owner == object ? bone_copies_moving : bone_copies_near);
  if (!file || bone % kBoneStride) return;
  const float local[3] = {GetFloat(base, matrix + 48), GetFloat(base, matrix + 52), GetFloat(base, matrix + 56)};
  float world[3];
  Through(base, m, local, world);
  bone_rows.push_back({owner, object, bone, Finite(world[0]), Finite(world[1]), Finite(world[2]), Finite(local[0]),
                       Finite(local[1]), Finite(local[2]), Finite(distance)});
}

int64_t Microseconds() {
  const int64_t now = platform::Ticks();
  if (!frequency) {
    frequency = platform::TickFrequency();
    start = now;
  }
  return (now - start) * 1000000 / frequency;
}

// The stage probe: the words of the view ([82709C40], kViewBytes from its
// start) and of the camera's copy (kCopy, kCopyBytes) that change between the
// stages of an iteration: the view block's start (82684050), the display's
// entry, the camera's entry and exit (8267A2A0), the display's exit and the
// delivery; counted apart for the iterations with and without a tick (the
// change from the delivery to the next view block counts for the next
// iteration).
enum Probe { kViewStart, kDisplayEntry, kCameraEntry, kCameraExit, kDisplayExit, kDelivery, kProbes };
constexpr const char* kProbeNames[kProbes] = {"view_start", "display_in", "camera_in", "camera_out", "display_out",
                                              "delivered"};
constexpr uint32_t kViewHolder = 0x82709C40, kViewBytes = 2600, kCopy = 0x82C0F1F8, kCopyBytes = 256;
constexpr uint32_t kProbeWords = (kViewBytes + kCopyBytes) / 4;
std::vector<uint8_t> probe_snapshot;
int probe_stage = -1;
uint64_t probe_words[2][kProbes][kProbes][kProbeWords]{};
uint64_t probe_compares[2][kProbes][kProbes]{};
std::string view_pointers;
void ProbeStage(uint8_t* base, Probe stage) {
  if (!Enabled()) return;
  const uint32_t view = Get32(base, kViewHolder);
  if (!view) return;
  std::vector<uint8_t> now(kViewBytes + kCopyBytes);
  if (!CopyFromGuest(GuestMemory(), view, now.data(), kViewBytes) ||
      !CopyFromGuest(GuestMemory(), kCopy, now.data() + kViewBytes, kCopyBytes))
    return;
  const int t = tick::Current().tick ? 1 : 0;
  if (probe_stage >= 0) {
    ++probe_compares[t][probe_stage][stage];
    for (uint32_t w = 0; w < kProbeWords; ++w)
      if (std::memcmp(now.data() + 4 * w, probe_snapshot.data() + 4 * w, 4)) ++probe_words[t][probe_stage][stage][w];
  }
  probe_snapshot.swap(now);
  probe_stage = stage;
  if (view_pointers.empty()) {  // The view's functions (8259E908 calls +24, +20 and +60) and the camera's call.
    const uint32_t manager = Get32(base, kCopy), table = manager ? Get32(base, manager) : 0;
    view_pointers = fmt::format("{{\"view\":\"{:08X}\",\"p20\":\"{:08X}\",\"p24\":\"{:08X}\",\"p28\":\"{:08X}\","
                                "\"p48\":\"{:08X}\",\"p60\":\"{:08X}\",\"manager\":\"{:08X}\",\"manager_12\":\"{:08X}\"}}",
                                view, Get32(base, view + 20), Get32(base, view + 24), Get32(base, view + 28),
                                Get32(base, view + 48), Get32(base, view + 60), manager,
                                table ? Get32(base, table + 12) : 0);
  }
}
// The probe's changed words as ranges [first byte, last byte, most changes], from first to end.
std::string ProbeRanges(const uint64_t* words, uint32_t first, uint32_t end) {
  std::string out;
  for (uint32_t w = first; w < end;) {
    if (!words[w]) {
      ++w;
      continue;
    }
    uint32_t stop = w;
    uint64_t most = 0;
    while (stop < end && words[stop]) most = std::max(most, words[stop++]);
    out += fmt::format("{}[{},{},{}]", out.empty() ? "" : ",", 4 * (w - first), 4 * (stop - first) - 1, most);
    w = stop;
  }
  return out;
}

void Display(PPCContext& ctx, uint8_t* base) {
  ProbeStage(base, kDisplayEntry);
  if (!Enabled()) {
    __imp__sub_8265A058(ctx, base);
    return;
  }
  const uint32_t world = ctx.r3.u32, view = ctx.r4.u32;
  const auto& f = tick::Current();
  ++displays;
  StageHash(base, kDisplayIn);
  const std::vector<uint32_t> live = LiveObjects(base, world);
  live_set.clear();
  live_set.insert(live.begin(), live.end());
  struct Moving { uint32_t object, matrix; float x, y, z, a, b, c; };
  std::vector<Moving> moving;
  for (const uint32_t object : live) {
    if (moving.size() >= 32 || !HasDynamics(base, object)) continue;
    const uint32_t m = Get32(base, object + 20);
    if (!m) continue;
    moving.push_back({object, m, Finite(GetFloat(base, m + 48)), Finite(GetFloat(base, m + 52)),
                      Finite(GetFloat(base, m + 56)), Finite(GetFloat(base, m)), Finite(GetFloat(base, m + 4)),
                      Finite(GetFloat(base, m + 8))});
  }
  // The matrix probe: raised during the display of an iteration without a tick.
  std::vector<std::pair<uint32_t, float>> raised;
  if (probe_units != 0.0f && f.gate && !f.tick)
    for (const auto& o : moving) {
      if (std::any_of(raised.begin(), raised.end(), [&](const auto& r) { return r.first == o.matrix + 52; }))
        continue;
      raised.emplace_back(o.matrix + 52, GetFloat(base, o.matrix + 52));
      PutFloat(base, o.matrix + 52, raised.back().second + probe_units);
    }
  mounted = mounted_outside = 0;
  moving_points.clear();
  for (const auto& o : moving) moving_points.push_back({o.object, o.x, o.y, o.z});
  bone_rows.clear();
  display_thread = GuestThread();
  in_display = true;
  __imp__sub_8265A058(ctx, base);
  in_display = false;
  for (const auto& [address, value] : raised) PutFloat(base, address, value);
  StageHash(base, kDisplayOut);
  ProbeStage(base, kDisplayExit);
  const auto& applied = interpolation::LastApplied();
  poses += applied.poses;
  bones_written += applied.bones;
  bone_cuts += applied.bone_cuts;
  bones_lost += applied.bones_lost;
  pose_other_thread += applied.pose_other_thread;
  if (applied.effects_wait_us) {
    ++effect_waits;
    effect_wait_us_sum += applied.effects_wait_us;
    effect_wait_us_max = std::max<uint64_t>(effect_wait_us_max, applied.effects_wait_us);
  }
  if (applied.effects_timeout) ++effect_wait_timeouts;
  if (!file) return;
  const int64_t us = Microseconds();
  const uint32_t cut = Get32(base, kCutCounter);
  const int camera_flags = (applied.camera ? 1 : 0) | (applied.camera_cut ? 4 : 0);
  std::string rows = fmt::format("{},{},{},{},{},{:.4f},{},F,{},{},{},{},{},{},{:.6f},{},{},{},{}\n", displays, us,
                                 f.index, int(f.gate), int(f.tick), f.alpha, cut, camera_flags, live.size(),
                                 moving.size(), mounted, mounted_outside, raised.size(), f.period, applied.objects,
                                 applied.object_cuts, applied.bones, applied.bone_cuts);
  if (view)
    rows += fmt::format("{},{},{},{},{},{:.4f},{},C,{:08X},{:.5f},{:.5f},{:.5f},{:.5f},{:.5f},{:.5f},0,0,0,0\n", displays,
                        us, f.index, int(f.gate), int(f.tick), f.alpha, cut, view, Finite(GetFloat(base, view + 476)),
                        Finite(GetFloat(base, view + 480)), Finite(GetFloat(base, view + 484)),
                        Finite(GetFloat(base, view + 460)), Finite(GetFloat(base, view + 464)),
                        Finite(GetFloat(base, view + 468)));
  for (const auto& o : moving)
    rows += fmt::format("{},{},{},{},{},{:.4f},{},O,{:08X},{:.5f},{:.5f},{:.5f},{:.5f},{:.5f},{:.5f},0,0,0,0\n", displays,
                        us, f.index, int(f.gate), int(f.tick), f.alpha, cut, o.object, o.x, o.y, o.z, o.a, o.b, o.c);
  for (const auto& b : bone_rows)
    rows += fmt::format("{},{},{},{},{},{:.4f},{},B,{:08X}/{:08X}/{},{:.5f},{:.5f},{:.5f},{:.5f},{:.5f},{:.5f},{:.4f},0,0,0\n",
                        displays, us, f.index, int(f.gate), int(f.tick), f.alpha, cut, b.owner, b.object, b.bone, b.x,
                        b.y, b.z, b.a, b.b, b.c, b.distance);
  std::fputs(rows.c_str(), file);
  if (displays % 60 == 0) std::fflush(file);
}

void Record(uint8_t* base) {
  const int64_t us = Microseconds();
  const double seconds = last_record ? double(us - last_record) * 1e-6 : 0.0;
  std::string pairs[2];
  size_t range_count = 0;
  uint64_t note_count = 0, distinct_count = 0, hashed = 0;
  std::string formats;
  {
    std::lock_guard lock(ranges_mutex);
    for (int t = 0; t < 2; ++t)
      for (int a = 0; a < kStages; ++a)
        for (int b = 0; b < kStages; ++b)
          if (changes[t][a][b])
            pairs[t] += fmt::format("{}\"{}-{}\":{}", pairs[t].empty() ? "" : ",", kStageNames[a], kStageNames[b],
                                    changes[t][a][b]);
    range_count = ranges.size();
    note_count = notes;
    distinct_count = distinct;
    hashed = hashed_ranges;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> kinds;  // (format, bytes) -> ranges
    for (const auto& [address, r] : ranges) ++kinds[{r.format, r.bytes}];
    for (const auto& [k, n] : kinds)
      formats += fmt::format("{}[{},{},{}]", formats.empty() ? "" : ",", k.first, k.second, n);
  }
  std::string effects;
  {
    std::lock_guard lock(effects_mutex);
    std::string updates;
    for (const auto& [fn, n] : effect_updates)
      updates += fmt::format("{}[\"{:08X}\",{}]", updates.empty() ? "" : ",", fn, n);
    effects = fmt::format(
        "\"runs\":{},\"view_ticks\":{},\"overlaps\":{},\"ms_mean\":{:.3f},\"ms_max\":{:.3f},"
        "\"overlap_ms_mean\":{:.3f},\"overlap_ms_max\":{:.3f},\"waits\":{},\"wait_ms_mean\":{:.3f},"
        "\"wait_ms_max\":{:.3f},\"wait_timeouts\":{},\"updates\":[{}]",
        effect_runs, view_ticks, effect_overlaps, effect_runs ? effect_ms_sum / double(effect_runs) : 0.0,
        effect_ms_max, effect_overlaps ? overlap_ms_sum / double(effect_overlaps) : 0.0, overlap_ms_max, effect_waits,
        effect_waits ? double(effect_wait_us_sum) / double(effect_waits) / 1000.0 : 0.0,
        double(effect_wait_us_max) / 1000.0, effect_wait_timeouts, updates);
  }
  std::string probe;
  for (int t = 1; t >= 0; --t)
    for (int x = 0; x < kProbes; ++x)
      for (int y = 0; y < kProbes; ++y) {
        if (!probe_compares[t][x][y]) continue;
        probe += fmt::format("{}{{\"tick\":{},\"from\":\"{}\",\"to\":\"{}\",\"compares\":{},\"view\":[{}],\"copy\":[{}]}}",
                             probe.empty() ? "" : ",", t, kProbeNames[x], kProbeNames[y], probe_compares[t][x][y],
                             ProbeRanges(probe_words[t][x][y], 0, kViewBytes / 4),
                             ProbeRanges(probe_words[t][x][y], kViewBytes / 4, kProbeWords));
      }
  const uint32_t time = Get32(base, 0x82C0F080);
  const uint32_t context = Get32(base, 0x82C0F008);
  host::Record("native-d3d-motion", fmt::format(
      "\"main_steps\":{},\"displays\":{},\"cameras\":{},\"seconds\":{:.3f},\"main_steps_per_s\":{:.3f},"
      "\"displays_per_s\":{:.3f},\"cameras_per_s\":{:.3f},\"fixed\":{},\"period\":{:.6f},\"title_interval\":{},"
      "\"tick\":{{\"index\":{},\"gate\":{},\"ticks\":{},\"skipped\":{}}},\"mounts\":{},\"probe_units\":{},"
      "\"vertex_textures\":{{\"ranges\":{},\"notes\":{},\"distinct\":{},\"hashed\":{},\"format_bytes_count\":[{}],"
      "\"changes_tick\":{{{}}},\"changes_without_tick\":{{{}}}}},\"probe\":{{\"pointers\":{},\"stages\":[{}]}},"
      "\"bones\":{{\"copies\":{},\"copies_outside_display\":{},\"copies_other_call\":{},\"copies_moving\":{},"
      "\"copies_near\":{},\"copies_far\":{},\"poses\":{},\"written\":{},\"cuts\":{},\"lost\":{},"
      "\"poses_other_thread\":{}}},\"effects\":{{{}}}",
      main_steps, displays, cameras, seconds, seconds > 0 ? double(main_steps - last_main_steps) / seconds : 0.0,
      seconds > 0 ? double(displays - last_displays) / seconds : 0.0,
      seconds > 0 ? double(cameras - last_cameras) / seconds : 0.0, time ? Get32(base, time + 72) : 0,
      time ? Finite(GetFloat(base, time + 76)) : 0.0f, context ? Get32(base, context + 448) : 0,
      tick::Current().index, tick::Current().gate, tick::Current().ticks, tick::Current().skipped, mounts, probe_units,
      range_count, note_count, distinct_count, hashed, formats, pairs[1], pairs[0],
      view_pointers.empty() ? std::string("null") : view_pointers, probe, bone_copies, bone_copies_outside,
      bone_copies_other, bone_copies_moving, bone_copies_near, bone_copies_far, poses, bones_written, bone_cuts,
      bones_lost, pose_other_thread, effects));
  last_record = us;
  last_displays = displays;
  last_cameras = cameras;
  last_main_steps = main_steps;
}
}  // namespace

bool Enabled() {
  if (enabled < 0) {
    const char* text = std::getenv("REX_NATIVE_MOTION_RECORD");
    enabled = Diagnostics() && text && std::string_view(text) == "1" ? 1 : 0;
    if (enabled) {
      if (const char* probe = std::getenv("REX_NATIVE_MATRIX_PROBE"); probe && *probe)
        probe_units = std::strtof(probe, nullptr);
      if (const char* dir = std::getenv("REX_NATIVE_FRAME_DIR"); dir && *dir) {
        file = std::fopen(fmt::format("{}/motion.csv", dir).c_str(), "w");
        if (file) std::fputs("display,us,iteration,gate,tick,alpha,cut,kind,id,x,y,z,a,b,c,d,e,f,g\n", file);
      }
    }
  }
  return enabled == 1;
}

void LoopStart(uint8_t* base) {
  if (!Enabled()) return;
  iteration_tick = tick::Current().tick;
  const uint64_t index = ++loop_index;
  {
    std::lock_guard lock(ranges_mutex);
    for (auto it = ranges.begin(); it != ranges.end();)
      it = index - it->second.seen > 300 ? ranges.erase(it) : std::next(it);
  }
  StageHash(base, kLoop);
}

void MainStepDone(uint8_t* base) {
  if (!Enabled()) return;
  if (++main_steps % 120 == 0) Record(base);
}

void ViewStart(uint8_t* base, uint32_t) {
  ProbeStage(base, kViewStart);
  if (!Enabled() || !tick::Current().gate || !tick::Current().tick) return;
  {
    std::lock_guard lock(effects_mutex);
    ++view_ticks;
  }
  view_tick_start = Qpc();
}

void Delivered(uint8_t* base) {
  view_tick_start = 0;
  if (!Enabled()) return;
  StageHash(base, kDelivered);
  ProbeStage(base, kDelivery);
}

void NoteMounted(uint32_t object) {
  if (!Enabled() || !in_display) return;
  ++mounted;
  ++mounts;
  if (!live_set.count(object)) ++mounted_outside;
}

void NoteVertexTexture(const std::array<uint32_t, 6>& d) {
  const uint32_t address = d[1] & 0xFFFFF000u, format = d[1] & 0x3F, dimension = (d[5] >> 9) & 3;
  const GuestFormat* guest = FindGuestFormat(format);
  if (!address || !guest || guest->block != 1) return;
  const uint64_t texels = dimension == 0 ? (d[2] & 0xFFFFFF) + 1ull
                                         : ((d[2] & 0x1FFF) + 1ull) * (((d[2] >> 13) & 0x1FFF) + 1ull);
  const uint32_t bytes = uint32_t(std::min<uint64_t>(texels * guest->bytes_per_block, kRangeBytesMax));
  std::lock_guard lock(ranges_mutex);
  ++notes;
  auto it = ranges.find(address);
  if (it == ranges.end()) {
    if (ranges.size() >= kRangesMax) return;
    ++distinct;
    it = ranges.emplace(address, Range{bytes, format}).first;
  }
  it->second.bytes = bytes;
  it->second.seen = loop_index.load();
}
}  // namespace gpu::motion

using namespace gpu;
using namespace gpu::motion;
REX_HOOK_RAW(sub_8265A058) { Display(ctx, base); }
REX_HOOK_RAW(sub_8267A2A0) {
  if (Enabled()) ++cameras;
  ProbeStage(base, kCameraEntry);
  __imp__sub_8267A2A0(ctx, base);
  ProbeStage(base, kCameraExit);
}
REX_HOOK_RAW(sub_820FC008) {
  if (Enabled()) StageHash(base, kListsIn);
  __imp__sub_820FC008(ctx, base);
  gpu::destruction_order::ListsDrawn();
  if (Enabled()) StageHash(base, kListsOut);
}
REX_HOOK_RAW(sub_826AB438) {
  gpu::sampler::RegisterThread("effects");  // The effects' work thread (82650708); REX_NATIVE_SAMPLE_PERIOD_US.
  narutorise::report_context::SetThreadRole("effects");
  if (Enabled()) EffectsUpdate(ctx, base);
  else __imp__sub_826AB438(ctx, base);
}
REX_HOOK_RAW(sub_820E91E0) {
  if (Enabled()) BoneCopy(base, ctx.r3.u32, ctx.r4.u32, uint32_t(ctx.lr));
  __imp__sub_820E91E0(ctx, base);
}
