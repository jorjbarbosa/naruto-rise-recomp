// Frame trace of the D3D boundary (frame_trace.h).
#include "gpu/common/platform.h"
#include "gpu/diagnostics/frame_trace.h"
#include "gpu/diagnostics/diagnostics.h"
#include "gpu/d3d/frame.h"
#include "gpu/common/guest_memory.h"
#include "gpu/host/services.h"
#include "gpu/backend/dispatcher.h"
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
FrameTrace frame_trace;
constexpr uint32_t kKeyTraces = 8;
void FrameTraceInit() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (!Diagnostics()) return;
    const char* text = std::getenv("REX_NATIVE_FRAME_TRACE");
    if (!text || !*text) return;
    if (std::string_view(text).substr(0, 6) == "scene+") frame_trace.scene_offset = std::strtoll(text + 6, nullptr, 10);
    else if (std::string_view(text) == "key") frame_trace.key_mode = true;
    else frame_trace.target.store(std::strtoull(text, nullptr, 10));
  });
}
bool FrameTraceActive() {
  FrameTraceInit();
  const uint64_t target = frame_trace.target.load(std::memory_order_relaxed);
  return target && swaps.load(std::memory_order_relaxed) + 1 == target;
}
uint64_t FrameTraceTarget() { FrameTraceInit(); return frame_trace.target.load(); }
// Called at each Swap n with the own draws of the frame it closes.
void FrameTraceNoteSwap(uint64_t n, uint64_t frame_draws) {
  FrameTraceInit();
  if (frame_trace.key_mode) {  // F10 arms the next frame.
    const bool down = platform::KeyDown(platform::Key::kF10);
    if (down && !frame_trace.key_down && !frame_trace.target.load() && frame_trace.key_traces < kKeyTraces) {
      ++frame_trace.key_traces;
      frame_trace.target.store(n + 1);
      host::Record("native-d3d-frame-trace-armed", fmt::format(
          "\"key\":\"F10\",\"trace\":{},\"swap\":{},\"frame_draws\":{},\"target_swap\":{}", frame_trace.key_traces, n,
          frame_draws, n + 1));
    }
    frame_trace.key_down = down;
  }
  if (frame_trace.scene_offset >= 0 && !frame_trace.scene_first && frame_draws >= 400) {
    frame_trace.scene_first = n;
    frame_trace.target.store(n + uint64_t(frame_trace.scene_offset));
    host::Record("native-d3d-frame-trace-armed", fmt::format(
        "\"scene_first_swap\":{},\"frame_draws\":{},\"target_swap\":{}", n, frame_draws, frame_trace.target.load()));
  }
}
// After a readback in the key mode, the trace is free for the next F10.
void FrameTraceRearm() {
  if (!frame_trace.key_mode) return;
  frame_trace.dests.clear();
  frame_trace.sampled.clear();
  frame_trace.pairs.clear();
  frame_trace.ops = 0;
  frame_trace.target.store(0);
}
void TraceFrameOp(const char* op, const std::string& fields) {
  host::Record("native-d3d-frame-trace", fmt::format(
      "\"swap\":{},\"n\":{},\"op\":\"{}\",\"thread\":{},{}", swaps.load() + 1, ++frame_trace.ops, op,
      GuestThread(), fields));
}
void TraceFrameResolveDest(uint32_t dest) {
  if (std::find(frame_trace.dests.begin(), frame_trace.dests.end(), dest) == frame_trace.dests.end())
    frame_trace.dests.push_back(dest);
}
// Frame trace: the front buffer and the resolve destinations of the traced
// frame, read back at its Swap (ReadTexture of the engine, after the frame's
// operations) and written as trace-<swap>-<key>.rgba.
void FrameTraceReadback(uint32_t front, uint64_t n) {
  std::vector<uint32_t> keys = frame_trace.dests;
  if (std::find(keys.begin(), keys.end(), front) == keys.end()) keys.push_back(front);
  std::string entries;
  for (uint32_t key : keys) {
    std::vector<uint8_t> rgba;
    uint32_t width = 0, height = 0;
    std::string evidence;
    auto* dispatcher = host::ActiveDispatcher();
    const bool read = dispatcher && dispatcher->Run([&](Engine& engine, std::string& out) {
      return engine.ReadTexture(key, rgba, width, height, out); }, evidence);
    uint64_t nonblack = 0, opaque = 0;
    for (size_t i = 0; i + 4 <= rgba.size(); i += 4) {
      nonblack += (rgba[i] | rgba[i + 1] | rgba[i + 2]) != 0;
      opaque += rgba[i + 3] == 255;
    }
    std::string file;
    if (const char* dir = std::getenv("REX_NATIVE_FRAME_DIR"); read && dir && *dir) {
      file = fmt::format("{}/trace-{:04}-{}-{}x{}.rgba", dir, n, key, width, height);
      std::FILE* out = std::fopen(file.c_str(), "wb");
      if (!out || std::fwrite(rgba.data(), 1, rgba.size(), out) != rgba.size()) file = "write-failed";
      if (out) std::fclose(out);
    }
    std::string file_json;
    for (char c : file) file_json += c == '\\' ? std::string("/") : std::string(1, c);
    entries += fmt::format("{}{{\"key\":{},\"front\":{},\"read\":{},\"size\":[{},{}],\"nonblack_pixels\":{},"
                           "\"opaque_pixels\":{},\"file\":\"{}\",\"engine\":{}}}", entries.empty() ? "" : ",", key,
                           key == front, read, width, height, nonblack, opaque, file_json,
                           evidence.empty() ? "null" : evidence);
  }
  // The sampled copies of the diagnosed draws, every level, as their rows:
  // trace-<swap>-tex-<key>-l<level>-f<format>-<w>x<h>-r<row bytes>.bin (the
  // number of the TextureFormat, contracts.h: the DXGI one).
  std::string sampled;
  for (const auto& [key, levels] : frame_trace.sampled) {
    for (uint32_t level = 0; level < levels; ++level) {
      std::vector<uint8_t> rows;
      TextureFormat format = TextureFormat::kUnknown;
      uint32_t width = 0, height = 0, row_bytes = 0;
      std::string evidence;
      auto* dispatcher = host::ActiveDispatcher();
      const bool read = dispatcher && dispatcher->Run([&](Engine& engine, std::string& out) {
        return engine.ReadSampled(key, level, rows, format, width, height, row_bytes, out); }, evidence);
      std::string file;
      if (const char* dir = std::getenv("REX_NATIVE_FRAME_DIR"); read && dir && *dir) {
        file = fmt::format("{}/trace-{:04}-tex-{}-l{}-f{}-{}x{}-r{}.bin", dir, n, key, level, uint32_t(format), width,
                           height, row_bytes);
        std::FILE* out = std::fopen(file.c_str(), "wb");
        if (!out || std::fwrite(rows.data(), 1, rows.size(), out) != rows.size()) file = "write-failed";
        if (out) std::fclose(out);
      }
      std::string file_json;
      for (char c : file) file_json += c == '\\' ? std::string("/") : std::string(1, c);
      sampled += fmt::format("{}{{\"key\":{},\"level\":{},\"file\":\"{}\",\"engine\":{}}}", sampled.empty() ? "" : ",",
                             key, level, file_json, evidence.empty() ? "null" : evidence);
      if (!read) break;
    }
  }
  host::Record("native-d3d-frame-trace-readback", fmt::format(
      "\"swap\":{},\"ops\":{},\"textures\":[{}],\"sampled\":[{}]", n, frame_trace.ops, entries, sampled));
}
}  // namespace gpu
