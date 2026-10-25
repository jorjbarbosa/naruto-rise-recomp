// The transforms between two logic ticks (transforms.h).
#include "gpu/interpolation/transforms.h"
#include "gpu/interpolation/tick.h"
#include "gpu/common/guest_memory.h"
#include "gpu/d3d/d3d.h"
#include "gpu/diagnostics/diagnostics.h"
#include "gpu/diagnostics/motion.h"
#include "gpu/host/services.h"
#include <windows.h>
#include <fmt/format.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" void __imp__sub_82684050(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8259FF40(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_825E2A08(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82659B88(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82580968(PPCContext& __restrict, uint8_t*);

namespace gpu::interpolation {
namespace {
constexpr uint32_t kCutCounter = 0x82C0EB34;
constexpr uint32_t kDisplayData = 0x82C0EFA4;  // The display's state: +208 its flags, +228 the object drawn.
// The world's effects: +8 their list, +160 the manager of their work thread.
// The logic posts the effects' update in the tick (82650610: the list at
// manager + 236, then the start event); the thread (82650708) runs it
// (826AB438) and clears +236 when done; the effects' draw waits for it inside
// the view block (826AC2E8).
constexpr uint32_t kWorldEffects = 0x82C0EB04;
constexpr auto kEffectsWaitMax = std::chrono::milliseconds(100);
constexpr float kCutDistance = 4.0f;    // Units in one tick (~120 units/s; walking is ~7).
constexpr float kCutCosine = 0.5f;      // A row turning more than 60 degrees in one tick.
constexpr float kCameraCutDistance = 10.0f, kCameraCutCosine = 0.866f;  // 8267A2A0.
// The camera: the matrix at the start of the view's entry (the 172-byte
// entries of world + 48), written by the logic in the tick. The display builds
// the view's camera from it (82659B88 -> 82584F58: the world matrix at view +
// 428, the view matrix at +496, the projection and the planes at +1084).
constexpr size_t kCameraFloats = 16;

using Matrix = std::array<float, 16>;
using Camera = std::array<float, kCameraFloats>;
// An engine matrix is 68 bytes: the 16 floats and a type word at +64. With
// kScaleFlag in it, the fourth column (+12, +28, +44) scales the rows. Without
// it, the renderer still copies the column into the full matrix inverted for
// lighting (821013E8 and 82100CF8). Keep the tick's values: forcing a scale of
// 1 into this column makes the light direction depend on the object position.
constexpr uint32_t kTypeOffset = 64, kScaleFlag = 8;
struct Track {
  uint32_t matrix = 0;
  uint64_t tick = 0;
  bool valid = false;
  Matrix previous{}, current{};
  uint32_t previous_type = 0, current_type = 0;
  // The display turned the written matrix in place (a billboard): what the
  // view block left on it (Restore), until the next write or tick. A matrix
  // shared by objects is written once per block, and this is on the track
  // that wrote it.
  bool turned = false;
  Matrix left{};
};
struct CameraTrack {
  uint32_t entry = 0;
  uint64_t tick = 0;
  bool valid = false, cut = false;
  Camera previous{}, current{};
};
// A bone matrix the pose copy reads (82580968), by its address.
struct BoneTrack {
  uint32_t owner = 0, bone = 0;  // The skinned object and the bone's object (0 for a local matrix).
  uint64_t tick = 0;
  bool valid = false;
  Matrix previous{}, current{};
};
// A place written over the live transform, put back after the delivery.
struct Written {
  uint32_t address = 0, count = 0;
  Matrix value{}, live{};
  uint32_t object = 0;  // An object matrix's object, 0 otherwise.
};

std::atomic<int> enabled{-1}, bones_enabled{-1};
int turned_enabled = -1;
std::unordered_map<uint32_t, Track> tracks;  // By object.
std::unordered_map<uint32_t, BoneTrack> bone_tracks;  // By matrix.
std::unordered_set<uint32_t> object_written;  // The object matrices written in this view block.
uint32_t main_thread = 0;  // The guest thread of the view block.
thread_local uint32_t pose_object = 0;  // The object of the running pose copy.
std::vector<uint32_t> mounted, candidates;   // Objects of the current and the last display.
bool collecting = false;
std::unordered_map<uint32_t, CameraTrack> cameras;  // By the view's entry.
std::unordered_set<uint32_t> camera_entries_done;   // The entries handled in this display.
std::vector<Written> written;         // The object matrices written in this view block.
std::vector<Written> camera_written;  // The camera entries written in this view block.
Applied applied;
std::atomic<uint32_t> mounts_other_thread{0};
// The pointers the engine freed (820C0018, any thread) since the last view
// block: an object freed by the tick (the deferred destruction at the end of
// the iteration, 82659060) leaves the candidates and the tracks before
// anything is written.
std::atomic<bool> watching{false};
std::mutex freed_mutex;
std::vector<uint32_t> freed;
bool freed_overflow = false;
constexpr size_t kFreedMax = 1 << 20;

// Guarded reads of guest memory: a pointer taken from an object the display
// mounted a tick ago, or from a bone array, may no longer point at mapped
// memory; CopyFromGuest fails instead of faulting.
bool Read32(uint8_t* base, uint32_t address, uint32_t& out) {
  if (!CopyFromGuest(&out, base + address, 4)) return false;
  out = _byteswap_ulong(out);
  return true;
}
bool ReadFloats(uint8_t* base, uint32_t address, float* out, size_t count) {
  if (!CopyFromGuest(out, base + address, 4 * count)) return false;
  for (size_t i = 0; i < count; ++i) {
    uint32_t v;
    std::memcpy(&v, out + i, 4);
    v = _byteswap_ulong(v);
    std::memcpy(out + i, &v, 4);
  }
  return true;
}
bool Finite(const float* v, size_t count) {
  return std::all_of(v, v + count, [](float f) { return std::isfinite(f); });
}
float Length3(const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
float Cosine3(const float* a, const float* b) {
  const float la = Length3(a), lb = Length3(b);
  if (la <= 1e-6f || lb <= 1e-6f) return 1.0f;
  return (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb);
}
float Distance3(const float* a, const float* b) {
  const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
  return Length3(d);
}
// A direction row between two: linear, with the length interpolated.
void BlendRow(const float* a, const float* b, float t, float* out) {
  for (int i = 0; i < 3; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
  const float length = Length3(out), target = Length3(a) + (Length3(b) - Length3(a)) * t;
  if (length > 1e-6f)
    for (int i = 0; i < 3; ++i) out[i] *= target / length;
}
void BlendMatrix(const float* a, const float* b, float t, float* out) {
  for (int row = 0; row < 3; ++row) {
    BlendRow(a + 4 * row, b + 4 * row, t, out + 4 * row);
    out[4 * row + 3] = a[4 * row + 3] + (b[4 * row + 3] - a[4 * row + 3]) * t;
  }
  for (int i = 12; i < 16; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
}
bool MatrixCut(const Matrix& a, const Matrix& b) {
  if (Distance3(a.data() + 12, b.data() + 12) > kCutDistance) return true;
  for (int row = 0; row < 3; ++row)
    if (Cosine3(a.data() + 4 * row, b.data() + 4 * row) < kCutCosine) return true;
  return false;
}
bool ReadCamera(uint8_t* base, uint32_t entry, Camera& out) { return ReadFloats(base, entry, out.data(), kCameraFloats); }
void WriteMatrix(uint8_t* base, uint32_t address, const Matrix& m) {
  for (uint32_t i = 0; i < 16; ++i) PutFloat(base, address + 4 * i, m[i]);
}
void WriteCamera(uint8_t* base, uint32_t entry, const Camera& c) {
  for (uint32_t i = 0; i < kCameraFloats; ++i) PutFloat(base, entry + 4 * i, c[i]);
}
// The position (row 3) and the rotation rows.
bool CameraCut(const Camera& a, const Camera& b) {
  if (Distance3(a.data() + 12, b.data() + 12) >= kCameraCutDistance) return true;
  for (int row = 0; row < 3; ++row)
    if (Cosine3(a.data() + 4 * row, b.data() + 4 * row) < kCameraCutCosine) return true;
  return false;
}
// The camera at the tick: the current one becomes the previous.
void TickCamera(CameraTrack& track, uint32_t entry, const Camera& live, uint64_t tick) {
  const bool continuing = track.entry == entry && track.tick + 1 == tick;
  track.previous = continuing ? track.current : live;
  track.current = live;
  track.cut = continuing && CameraCut(track.previous, track.current);
  // Both ends finite: a NaN previous passes the cuts (their comparisons are false).
  track.valid = continuing && !track.cut && Finite(track.previous.data(), kCameraFloats) &&
                Finite(live.data(), kCameraFloats);
  track.entry = entry;
  track.tick = tick;
}
// Writes the camera of the track at alpha; false when it does not apply.
bool ApplyCamera(uint8_t* base, CameraTrack& track, uint32_t entry, uint64_t tick, float alpha) {
  if (!track.valid || track.entry != entry || track.tick != tick || Get32(base, kCutCounter)) return false;
  if (track.previous == track.current) return false;
  Camera live;
  if (!ReadCamera(base, entry, live) || live != track.current) {  // Something else wrote it since the tick.
    track.valid = false;
    return false;
  }
  Written w;
  w.address = entry;
  w.count = kCameraFloats;
  BlendMatrix(track.previous.data(), track.current.data(), alpha, w.value.data());
  w.live = track.current;
  WriteCamera(base, entry, w.value);
  camera_written.push_back(w);
  return true;
}

bool Active() { return gpu::Active() && Enabled() && tick::InGatedStep(); }

bool BonesEnabled() {
  if (bones_enabled < 0) {
    const char* text = std::getenv("REX_NATIVE_INTERPOLATION_BONES");
    bones_enabled = Enabled() && !(text && std::string_view(text) == "0") ? 1 : 0;
  }
  return bones_enabled == 1;
}

// The matrices the display turns in place (transforms.h):
// REX_NATIVE_INTERPOLATION_TURNED=0 leaves them as the view block left them;
// with the diagnostics, F7 switches it at run time.
bool TurnedEnabled() {
  if (turned_enabled < 0) {
    const char* text = std::getenv("REX_NATIVE_INTERPOLATION_TURNED");
    turned_enabled = !(text && std::string_view(text) == "0") ? 1 : 0;
  }
  return turned_enabled == 1;
}
bool f7_down = false;
void SwitchTurned() {
  if (!Diagnostics()) return;
  const bool down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
  if (down && !f7_down) {
    turned_enabled = TurnedEnabled() ? 0 : 1;
    host::Record("native-d3d-interpolation-turned-switch", fmt::format("\"on\":{}", turned_enabled == 1));
  }
  f7_down = down;
}
// Since the start, with the diagnostics a native-d3d-interpolation-turned
// record every kTurnedRecordEvery view blocks: the object matrices written,
// those the view block changed, those only turned (given the tick's scale and
// position back), turned ones written again in a frame without a tick, and
// those written outside the logic (lost).
constexpr uint64_t kTurnedRecordEvery = 600;
struct TurnedCounts {
  uint64_t blocks = 0, written = 0, changed = 0, turned = 0, rewritten = 0, lost = 0;
} turned_counts;
void RecordTurned() {
  if (++turned_counts.blocks % kTurnedRecordEvery || !Diagnostics()) return;
  const auto& c = turned_counts;
  host::Record("native-d3d-interpolation-turned", fmt::format(
      "\"blocks\":{},\"written\":{},\"changed\":{},\"turned\":{},\"rewritten\":{},\"lost\":{},\"on\":{}", c.blocks,
      c.written, c.changed, c.turned, c.rewritten, c.lost, TurnedEnabled()));
}

// Puts back the live value where nothing else wrote since; false where
// something did.
bool PutBack(uint8_t* base, const Written& w) {
  for (uint32_t i = 0; i < w.count; ++i)
    if (GetFloat(base, w.address + 4 * i) != w.value[i]) return false;
  for (uint32_t i = 0; i < w.count; ++i) PutFloat(base, w.address + 4 * i, w.live[i]);
  return true;
}
void PutBack(uint8_t* base, const std::vector<Written>& list) {
  for (const auto& w : list) PutBack(base, w);
}

// The bone matrices the pose copy of a skinned mesh reads (82580968, the
// object and its geometry), with the bone's object (0 for a local matrix):
// the skin [geometry + 60] (+2 its bones, +4 their entries: +0 the bone, +72
// its vertices), copied only with the flags of the display (+208 0x20000), of
// the object (0x200000) and of the geometry (+84 bit 3). The bones are
// [[object + 28] + 12] (+0 their count, +4 the array). With the object's flag
// 0x01000000 the array has 12-byte pointers (+0 a matrix, +4 the bone's
// object, +8 -1 or an id): the bone's global matrix [bone + 20], relative to
// the object's (when +8 is -1 or the bone's object has the same flag), or the
// matrix at +0; without it, 68-byte matrices relative to the object.
void BoneMatrices(uint8_t* base, uint32_t object, uint32_t geometry, std::vector<std::pair<uint32_t, uint32_t>>& out) {
  const uint32_t skin = Get32(base, geometry + 60), display = Get32(base, kDisplayData);
  const uint32_t flags = Get32(base, object);
  if (!skin || !display || !(Get32(base, display + 208) & 0x20000) || !(flags & 0x200000) ||
      !(Get32(base, geometry + 84) & 8))
    return;
  const uint32_t extension = Get32(base, object + 28);
  const uint32_t bones = extension ? Get32(base, extension + 12) : 0;
  if (!bones) return;
  const uint32_t count = Get32(base, bones), array = Get32(base, bones + 4);
  const uint32_t entries = Get32(base, skin + 4), n = Get16(base, skin + 2);
  if (!array || !entries) return;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t entry = Get32(base, entries + 4 * i);
    if (!entry || !Get16(base, entry + 72)) continue;
    const uint32_t index = Get16(base, entry);
    if (index >= count) continue;
    if (!(flags & 0x01000000)) {
      out.emplace_back(array + 68 * index, 0);
      continue;
    }
    const uint32_t pointer = array + 12 * index, bone = Get32(base, pointer + 4);
    if (Get32(base, pointer + 8) == 0xFFFFFFFFu || (bone && (Get32(base, bone) & 0x01000000))) {
      if (const uint32_t m = bone ? Get32(base, bone + 20) : 0) out.emplace_back(m, bone);
    } else if (const uint32_t m = Get32(base, pointer)) {
      out.emplace_back(m, bone);
    }
  }
}

// The pose of a skinned mesh (82580968), before the display copies it into
// the frame's packet: the bone matrices at the tick's alpha (out, put back
// right after the copy).
void Pose(uint8_t* base, uint32_t object, uint32_t geometry, std::vector<Written>& out) {
  if (!collecting || !BonesEnabled()) return;
  if (GuestThread() != main_thread) {
    ++applied.pose_other_thread;
    return;
  }
  ++applied.poses;
  static std::vector<std::pair<uint32_t, uint32_t>> bones;  // Main thread (above): a scratch kept across the calls.
  bones.clear();
  BoneMatrices(base, object, geometry, bones);
  const auto& f = tick::Current();
  for (const auto& [m, bone] : bones) {
    if (object_written.count(m) ||
        std::any_of(out.begin(), out.end(), [&](const Written& w) { return w.address == m; }))
      continue;
    Matrix live;
    if (!ReadFloats(base, m, live.data(), 16)) continue;
    BoneTrack& t = bone_tracks[m];
    if (f.tick && t.tick != f.ticks) {
      const bool continuing = t.tick + 1 == f.ticks && t.owner == object && t.bone == bone;
      t.previous = continuing ? t.current : live;
      t.current = live;
      const bool cut = continuing && MatrixCut(t.previous, t.current);
      if (cut) ++applied.bone_cuts;
      t.valid = continuing && !cut && Finite(t.previous.data(), 16) && Finite(live.data(), 16);
      t.owner = object;
      t.bone = bone;
      t.tick = f.ticks;
    }
    if (!t.valid || t.tick != f.ticks || t.previous == t.current) continue;
    if (live != t.current) {  // Written outside the logic since the tick.
      t.valid = false;
      ++applied.bones_lost;
      continue;
    }
    Written w;
    w.address = m;
    w.count = 16;
    BlendMatrix(t.previous.data(), t.current.data(), f.alpha, w.value.data());
    std::copy(live.begin(), live.end(), w.live.begin());
    WriteMatrix(base, m, w.value);
    out.push_back(w);
    ++applied.bones;
  }
}

// In an iteration with a tick, the effects' update may still run on its work
// thread when the view block starts: the block's writes (the interpolated
// matrices, the view block's dt of tick.h) wait for it, so the effects keep
// reading the tick's state. +236 is read as written by the other thread; the
// wait is bounded (kEffectsWaitMax).
uint32_t effects_wait_us = 0;
bool effects_timeout = false;
void SettleEffects(uint8_t* base) {
  effects_wait_us = 0;
  effects_timeout = false;
  if (!gpu::Active() || !tick::InGatedStep() || !tick::Current().tick) return;
  uint32_t effects = 0, manager = 0;
  if (!Read32(base, kWorldEffects, effects) || !effects || !Read32(base, effects + 160, manager) || !manager) return;
  const volatile uint32_t* list = reinterpret_cast<const volatile uint32_t*>(base + manager + 236);
  if (!*list) return;
  const auto start = std::chrono::steady_clock::now();
  while (*list) {
    if (std::chrono::steady_clock::now() - start > kEffectsWaitMax) {
      effects_timeout = true;
      break;
    }
    std::this_thread::yield();
  }
  effects_wait_us = uint32_t(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
}

// The start of the view block (82684050, r3 the view).
void ViewStart(uint8_t* base) {
  candidates.swap(mounted);
  mounted.clear();
  collecting = Active();
  applied = {};
  applied.effects_wait_us = effects_wait_us;
  applied.effects_timeout = effects_timeout;
  object_written.clear();
  camera_entries_done.clear();
  if (!collecting) {
    tracks.clear();
    bone_tracks.clear();
    cameras.clear();
    watching = false;
    std::lock_guard lock(freed_mutex);
    freed.clear();
    return;
  }
  main_thread = GuestThread();
  watching = true;
  SwitchTurned();
  {
    std::vector<uint32_t> gone;
    bool overflow = false;
    {
      std::lock_guard lock(freed_mutex);
      gone.swap(freed);
      overflow = freed_overflow;
      freed_overflow = false;
    }
    if (overflow) {
      tracks.clear();
      bone_tracks.clear();
      candidates.clear();
    } else if (!gone.empty() && !(tracks.empty() && bone_tracks.empty() && candidates.empty())) {
      const std::unordered_set<uint32_t> set(gone.begin(), gone.end());
      for (auto it = tracks.begin(); it != tracks.end();)
        it = set.count(it->first) || set.count(it->second.matrix) ? tracks.erase(it) : std::next(it);
      for (auto it = bone_tracks.begin(); it != bone_tracks.end();)
        it = set.count(it->first) || set.count(it->second.owner) || set.count(it->second.bone) ? bone_tracks.erase(it)
                                                                                                : std::next(it);
      std::erase_if(candidates, [&](uint32_t object) { return set.count(object) != 0; });
    }
  }
  const auto& f = tick::Current();
  const uint64_t tick = f.ticks;
  if (f.tick) {
    std::unordered_set<uint32_t> seen;
    for (const uint32_t object : candidates) {
      if (!seen.insert(object).second) continue;
      // An object freed since the display by another path than 820C0018 may
      // hold anything at +20: the reads are guarded, and the track stays at
      // its tick (no write, gone after two ticks).
      uint32_t m = 0, type = 0;
      Matrix live;
      if (!Read32(base, object + 20, m) || !m || !ReadFloats(base, m, live.data(), 16) ||
          !Read32(base, m + kTypeOffset, type))
        continue;
      Track& t = tracks[object];
      const bool continuing = t.matrix == m && t.tick + 1 == tick;
      t.previous = continuing ? t.current : live;
      t.current = live;
      t.previous_type = continuing ? t.current_type : type;
      t.current_type = type;
      t.turned = false;
      const bool cut = continuing && MatrixCut(t.previous, t.current);
      if (cut) ++applied.object_cuts;
      t.valid = continuing && !cut && Finite(t.previous.data(), 16) && Finite(live.data(), 16);
      t.matrix = m;
      t.tick = tick;
    }
    for (auto it = tracks.begin(); it != tracks.end();) it = it->second.tick + 2 < tick ? tracks.erase(it) : std::next(it);
    for (auto it = bone_tracks.begin(); it != bone_tracks.end();)
      it = it->second.tick + 2 < tick ? bone_tracks.erase(it) : std::next(it);
    for (auto it = cameras.begin(); it != cameras.end();) it = it->second.tick + 2 < tick ? cameras.erase(it) : std::next(it);
  }
  const float alpha = f.alpha;
  std::unordered_set<uint32_t> matrices;
  for (auto& [object, t] : tracks) {
    if (!t.valid || t.tick != tick || t.previous == t.current || !matrices.insert(t.matrix).second) continue;
    // The fourth column changes meaning with the scale flag. Keep the tick's
    // matrix during that transition (including billboards reaching full size).
    if ((t.previous_type ^ t.current_type) & kScaleFlag) continue;
    // In a frame without a tick, a matrix the display turned holds the turn
    // the last view block left (Restore): the interpolated value goes under
    // it again, as the display ran once per state of the logic on the console.
    Matrix live;
    if (!ReadFloats(base, t.matrix, live.data(), 16)) {
      t.valid = false;
      continue;
    }
    const bool turned = TurnedEnabled() && t.turned && live == t.left;
    if (live != t.current && !turned) {  // Written outside the logic since the tick.
      t.valid = false;
      ++turned_counts.lost;
      continue;
    }
    if (turned) ++turned_counts.rewritten;
    t.turned = false;
    Written w;
    w.address = t.matrix;
    w.count = 16;
    w.object = object;
    BlendMatrix(t.previous.data(), t.current.data(), alpha, w.value.data());
    std::copy(t.current.begin(), t.current.end(), w.live.begin());
    WriteMatrix(base, w.address, w.value);
    written.push_back(w);
    object_written.insert(w.address);
    ++applied.objects;
  }
}

// The display's camera (82659B88, r3 the view's entry), before it builds the
// view's camera from the entry: once per display and entry (the display calls
// it again at its end, over the same entry), with a track per entry.
void CameraEntry(uint8_t* base, uint32_t entry) {
  if (!collecting || !entry || !camera_entries_done.insert(entry).second) return;
  const auto& f = tick::Current();
  CameraTrack& camera = cameras[entry];
  if (f.tick) {
    Camera live;
    if (!ReadCamera(base, entry, live)) {
      cameras.erase(entry);
      return;
    }
    TickCamera(camera, entry, live, f.ticks);
    applied.camera_cut = applied.camera_cut || camera.cut;
  }
  applied.camera = ApplyCamera(base, camera, entry, f.ticks, f.alpha) || applied.camera;
}

// After the delivery: the live transforms come back where nothing else wrote.
// Where the display only turned a written object matrix (a billboard: the
// scale column and the position as written), the tick's scale and position go
// back under the turn: the logic of the next tick reads the matrix (the
// indicators over the characters animate their scale from it), and on the
// console it read its own values turned by the display.
void Restore(uint8_t* base) {
  turned_counts.written += written.size();
  for (const auto& w : written) {
    if (PutBack(base, w)) continue;
    ++turned_counts.changed;
    Matrix now;
    bool turned = TurnedEnabled() && ReadFloats(base, w.address, now.data(), 16);
    for (int row = 0; row < 3; ++row) turned = turned && now[4 * row + 3] == w.value[4 * row + 3];
    for (int i = 12; i < 16; ++i) turned = turned && now[i] == w.value[i];
    const auto it = tracks.find(w.object);
    if (!turned || it == tracks.end() || it->second.matrix != w.address) continue;
    for (int row = 0; row < 3; ++row) now[4 * row + 3] = w.live[4 * row + 3];
    for (int i = 12; i < 16; ++i) now[i] = w.live[i];
    WriteMatrix(base, w.address, now);
    it->second.turned = true;
    it->second.left = now;
    ++turned_counts.turned;
  }
  written.clear();
  RecordTurned();
  PutBack(base, camera_written);
  camera_written.clear();
  collecting = false;
}
}  // namespace

bool Enabled() {
  if (enabled < 0) {
    const char* text = std::getenv("REX_NATIVE_INTERPOLATION_TRANSFORMS");
    enabled = tick::Enabled() && !(text && std::string_view(text) == "0") ? 1 : 0;
  }
  return enabled == 1;
}

const Applied& LastApplied() { return applied; }
void ResetHistory(uint8_t* base) {
  Restore(base);
  tracks.clear(); bone_tracks.clear(); cameras.clear();
  mounted.clear(); candidates.clear(); object_written.clear(); camera_entries_done.clear();
  applied = {};
  enabled = bones_enabled = -1;
  watching.store(false);
  std::lock_guard lock(freed_mutex);
  freed.clear(); freed_overflow = false;
}

uint32_t PoseObject() { return pose_object; }

uint32_t MountsOtherThread() { return mounts_other_thread.load(); }

void NoteFree(uint32_t pointer) {
  if (!watching.load(std::memory_order_relaxed)) return;
  std::lock_guard lock(freed_mutex);
  if (freed.size() < kFreedMax) freed.push_back(pointer);
  else freed_overflow = true;
}
}  // namespace gpu::interpolation

// The view block's start and the delivery, only from the main step: 82684050
// is also called by 8267F078 and 82684470, and 8259FF40 by 82684470 and
// 82685AB8; a call from there would clear the tracks.
REX_HOOK_RAW(sub_82684050) {
  if (!gpu::tick::InMainStep()) {
    __imp__sub_82684050(ctx, base);
    return;
  }
  gpu::interpolation::SettleEffects(base);   // Before any write of the view block.
  gpu::motion::ViewStart(base, ctx.r3.u32);  // The view probe, before the writes.
  gpu::tick::BeginView(base);
  gpu::interpolation::ViewStart(base);
  __imp__sub_82684050(ctx, base);
}
REX_HOOK_RAW(sub_8259FF40) {
  __imp__sub_8259FF40(ctx, base);
  if (!gpu::tick::InMainStep()) return;
  gpu::tick::EndView(base);
  gpu::interpolation::Restore(base);
  gpu::motion::Delivered(base);
}
// The mount of the display's lists: the objects go to the tracks only from
// the view block's thread (825E0758 and 825E1918 also call it, with no
// static caller); a mount from another thread is counted, not tracked.
REX_HOOK_RAW(sub_825E2A08) {
  using namespace gpu::interpolation;
  const bool tracking = collecting && gpu::GuestThread() == main_thread;
  if (collecting && !tracking) ++mounts_other_thread;
  if (tracking || gpu::motion::Enabled()) {
    const uint32_t holder = ctx.r3.u32, count = gpu::Get32(base, holder + 8), array = gpu::Get32(base, holder + 4);
    for (uint32_t i = 0; array && i < count && i < 65536; ++i) {
      const uint32_t object = gpu::Get32(base, array + 4 * i);
      if (!object) continue;
      const uint32_t flags = gpu::Get32(base, object + 4);
      if (!(flags & 0x00040000) || (flags & 0x00200004)) continue;
      if (tracking) mounted.push_back(object);
      gpu::motion::NoteMounted(object);
    }
  }
  __imp__sub_825E2A08(ctx, base);
}
REX_HOOK_RAW(sub_82659B88) {
  gpu::interpolation::CameraEntry(base, ctx.r3.u32);
  __imp__sub_82659B88(ctx, base);
}
REX_HOOK_RAW(sub_82580968) {
  std::vector<gpu::interpolation::Written> pose;
  gpu::interpolation::Pose(base, ctx.r3.u32, ctx.r4.u32, pose);
  gpu::interpolation::pose_object = ctx.r3.u32;
  __imp__sub_82580968(ctx, base);
  gpu::interpolation::pose_object = 0;
  gpu::interpolation::PutBack(base, pose);
}
