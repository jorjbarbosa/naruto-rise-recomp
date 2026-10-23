// Front-buffer captures of the own Swap (frame_capture.h).
#include "gpu/common/platform.h"
#include "gpu/diagnostics/frame_capture.h"
#include "gpu/diagnostics/diagnostics.h"
#include "gpu/textures/textures.h"
#include "gpu/backend/dispatcher.h"
#include "gpu/host/services.h"
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
#include <functional>
#include <thread>
#include <rex/runtime.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

using namespace gpu;

namespace gpu {

// Own presentation evidence: the own texture of the front buffer, read back and
// written as raw RGBA to REX_NATIVE_FRAME_DIR.
std::string ReadFrontBuffer(uint32_t front, uint64_t n) {
  std::vector<uint8_t> rgba;
  uint32_t width = 0, height = 0;
  std::string evidence;
  auto* dispatcher = host::ActiveDispatcher();
  const bool read = dispatcher && dispatcher->Run([&](Engine& engine, std::string& out) {
    return engine.ReadTexture(front, rgba, width, height, out); }, evidence);
  if (!read) return fmt::format("{{\"read\":false,\"engine\":{}}}", evidence.empty() ? "null" : evidence);
  uint64_t hash = 0xCBF29CE484222325ull, opaque = 0, nonblack = 0;
  for (uint8_t b : rgba) hash = (hash ^ b) * 0x100000001B3ull;
  for (size_t i = 0; i + 4 <= rgba.size(); i += 4) {
    nonblack += (rgba[i] | rgba[i + 1] | rgba[i + 2]) != 0;
    opaque += rgba[i + 3] == 255;
  }
  std::string file;
  if (const char* dir = std::getenv("REX_NATIVE_FRAME_DIR"); dir && *dir) {
    file = fmt::format("{}/swap-{:04}-{}x{}.rgba", dir, n, width, height);
    std::FILE* out = std::fopen(file.c_str(), "wb");
    if (!out || std::fwrite(rgba.data(), 1, rgba.size(), out) != rgba.size()) file = "write-failed";
    if (out) std::fclose(out);
  }
  std::string file_json;
  for (char c : file) file_json += c == '\\' ? std::string("/") : std::string(1, c);
  const auto pixel = [&](uint32_t x, uint32_t y) {
    const size_t i = (size_t(y) * width + x) * 4;
    return fmt::format("[{},{},{},{}]", rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]);
  };
  return fmt::format("{{\"read\":true,\"fnv64\":\"{:016x}\",\"nonblack_pixels\":{},\"opaque_pixels\":{},"
                     "\"center\":{},\"corner\":{},\"file\":\"{}\",\"engine\":{}}}", hash, nonblack, opaque,
                     pixel(width / 2, height / 2), pixel(0, 0), file_json, evidence);
}

// REX_NATIVE_TRANSITION_CAPTURES=<period>
// reads the front buffer back at every <period>-th own Swap and at each scene
// change and 1, 3, 10 and 30 Swaps after it, reduced 4 x 4 (box;
// REX_NATIVE_TRANSITION_REDUCE=1 or 2 for a finer one) and written as
// transition-NNNNN-WxH.rgba with a native-d3d-transition-capture record, at
// most kTransitionCapturesMax. A scene change is a Swap whose frame moved to
// another draw class (videos only: at most 8 own draws; menus: below 400; the
// Story Mode scene) or forgot a texture base (D750), or registered at least
// kTransitionRegistrations texture entries. Unset or 0: off. Evidence only:
// each read waits for the GPU. Render thread only (own Swap).
uint64_t TransitionPeriod() {
  static const uint64_t value = [] {
    if (!Diagnostics()) return uint64_t(0);
    const char* text = std::getenv("REX_NATIVE_TRANSITION_CAPTURES");
    return text && *text ? uint64_t(std::strtoull(text, nullptr, 10)) : uint64_t(0);
  }();
  return value;
}
uint32_t TransitionReduce() {
  static const uint32_t value = [] {
    const char* text = std::getenv("REX_NATIVE_TRANSITION_REDUCE");
    const uint32_t n = text && *text ? uint32_t(std::strtoul(text, nullptr, 10)) : 4u;
    return n == 1 || n == 2 ? n : 4u;
  }();
  return value;
}
constexpr uint64_t kTransitionCapturesMax = 1200, kTransitionRegistrations = 8;
struct TransitionState {
  int draw_class = -1;
  uint64_t registered = 0, forgotten = 0, taken = 0, changes = 0, failed = 0;
  std::map<uint64_t, std::string> pending;  // Swap -> reason.
  // F9 captures the next kKeyBurstFrames Swaps in a row (a frozen image
  // repeats its hash), at most kKeyBursts times.
  bool f9_down = false;
  uint32_t bursts = 0;
} transitions;
constexpr uint64_t kKeyBurstFrames = 90;
constexpr uint32_t kKeyBursts = 6;
void TransitionCaptureAtSwap(uint32_t front, uint64_t n, uint64_t frame_draws) {
  if (!TransitionPeriod()) return;
  auto& t = transitions;
  const auto totals = TextureRegistrySnapshot();
  const int draw_class = frame_draws <= 8 ? 0 : frame_draws < 400 ? 1 : 2;
  std::string change;
  if (t.draw_class >= 0 && draw_class != t.draw_class) change = fmt::format("class-{}-{}", t.draw_class, draw_class);
  else if (totals.forgotten_bases > t.forgotten) change = fmt::format("forgot-{}", totals.forgotten_bases - t.forgotten);
  else if (totals.registered >= t.registered + kTransitionRegistrations)
    change = fmt::format("registered-{}", totals.registered - t.registered);
  t.draw_class = draw_class;
  t.registered = totals.registered;
  t.forgotten = totals.forgotten_bases;
  if (!change.empty()) {
    ++t.changes;
    for (uint64_t k : {0, 1, 3, 10, 30}) t.pending.emplace(n + k, fmt::format("{}+{}", change, k));
  }
  const bool f9 = platform::KeyDown(platform::Key::kF9);  // A burst of consecutive captures.
  if (f9 && !t.f9_down && t.bursts < kKeyBursts) {
    ++t.bursts;
    for (uint64_t k = 0; k < kKeyBurstFrames; ++k)
      t.pending.insert_or_assign(n + k, fmt::format("key{}+{}", t.bursts, k));
  }
  t.f9_down = f9;
  std::string reason;
  if (auto it = t.pending.find(n); it != t.pending.end()) reason = it->second;
  else if (n % TransitionPeriod() == 0) reason = "periodic";
  while (!t.pending.empty() && t.pending.begin()->first <= n) t.pending.erase(t.pending.begin());
  if (reason.empty() || t.taken >= kTransitionCapturesMax) return;
  ++t.taken;
  std::vector<uint8_t> rgba;
  uint32_t width = 0, height = 0;
  std::string evidence;
  auto* dispatcher = host::ActiveDispatcher();
  const bool read = dispatcher && dispatcher->Run([&](Engine& engine, std::string& out) {
    return engine.ReadTexture(front, rgba, width, height, out); }, evidence);
  const uint32_t r = TransitionReduce(), w = width / r, h = height / r;
  std::string file = "null";
  uint64_t hash = 0xCBF29CE484222325ull;
  if (read && w && h && rgba.size() >= size_t(width) * height * 4) {
    std::vector<uint8_t> reduced(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x)
        for (uint32_t c = 0; c < 4; ++c) {
          uint32_t sum = 0;
          for (uint32_t dy = 0; dy < r; ++dy)
            for (uint32_t dx = 0; dx < r; ++dx) sum += rgba[((size_t(y) * r + dy) * width + x * r + dx) * 4 + c];
          reduced[(size_t(y) * w + x) * 4 + c] = uint8_t((sum + r * r / 2) / (r * r));
        }
    for (uint8_t v : reduced) hash = (hash ^ v) * 0x100000001B3ull;
    const std::string name = fmt::format("transition-{:05}-{}x{}.rgba", n, w, h);
    if (const char* dir = std::getenv("REX_NATIVE_FRAME_DIR"); dir && *dir) {
      std::FILE* out = std::fopen(fmt::format("{}/{}", dir, name).c_str(), "wb");
      file = out && std::fwrite(reduced.data(), 1, reduced.size(), out) == reduced.size() ? "\"" + name + "\""
                                                                                    : std::string("\"write-failed\"");
      if (out) std::fclose(out);
    }
  } else {
    ++t.failed;
  }
  host::Record("native-d3d-transition-capture", fmt::format(
      "\"swap\":{},\"reason\":\"{}\",\"frame_draws\":{},\"texture_bases\":{},\"textures_registered\":{},"
      "\"texture_bases_forgotten\":{},\"read\":{},\"size\":[{},{}],\"fnv64\":\"{:016x}\",\"file\":{},\"engine\":{}",
      n, reason, frame_draws, totals.bases, totals.registered, totals.forgotten_bases, read, w, h, hash, file,
      read ? std::string("null") : evidence.empty() ? std::string("null") : evidence));
}
std::string TransitionSummary() {
  return fmt::format("\"period\":{},\"changes\":{},\"captures\":{},\"failed\":{},\"max\":{}", TransitionPeriod(),
                     transitions.changes, transitions.taken, transitions.failed, kTransitionCapturesMax);
}
}  // namespace gpu
