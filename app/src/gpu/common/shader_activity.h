// Read-only UI telemetry of pipeline compilation; never changes scheduling.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
namespace gpu {
inline std::atomic<uint32_t> shader_jobs{0};
inline std::atomic<int64_t> shader_last_finished{0};
inline std::atomic<uint32_t> shader_batch_total{0}, shader_batch_done{0};
inline std::atomic<bool> shader_batch_running{false};
inline void BeginShaderBatch(uint32_t count) { shader_batch_done = 0; shader_batch_total = count; shader_batch_running = true; }
inline void EndShaderBatch() { shader_batch_running = false; }
struct ShaderBatchEntry { ~ShaderBatchEntry() { shader_batch_done.fetch_add(1, std::memory_order_relaxed); } };
struct ShaderProgress { uint32_t pending; float fraction; };
inline ShaderProgress CurrentShaderProgress() {
  const uint32_t active = shader_jobs.load(std::memory_order_relaxed);
  if (!shader_batch_running.load()) return {active, active ? -1.0f : 1.0f};
  const auto total = shader_batch_total.load(), done = shader_batch_done.load();
  return {total > done ? total - done : active, total ? float(done) / float(total) : 1.0f};
}
inline int64_t ShaderClock() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline void NoteShaderCompiled() { shader_last_finished.store(ShaderClock(), std::memory_order_relaxed); }
struct ShaderCompilation {
  ShaderCompilation() { shader_jobs.fetch_add(1, std::memory_order_relaxed); }
  ~ShaderCompilation() { NoteShaderCompiled(); shader_jobs.fetch_sub(1, std::memory_order_relaxed); }
};
inline bool ShaderCompilationVisible() {
  const auto last = shader_last_finished.load(std::memory_order_relaxed);
  return shader_jobs.load(std::memory_order_relaxed) != 0 || (last != 0 && ShaderClock() - last < 750);
}
}  // namespace gpu
