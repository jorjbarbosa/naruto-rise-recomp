// What every backend (backend.h) does the same way in its owner's life: the
// render mode Initialize accepts and the sample count it lowers to the
// device's, the epoch of each engine, and the evidence of a frontend
// generation that is ready or released.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <fmt/format.h>
#include <rex/logging.h>
#include "diagnostics/incident.h"
#include "gpu/backend/contracts.h"

namespace gpu {

// The end of an owner whose release could not prove the GPU done with its
// resources: they cannot be destroyed, so the process ends, with the reason in
// the log and the incident channel rather than silently.
[[noreturn]] inline void ExitUnreleased(std::string_view where, std::string_view evidence) {
  narutorise::incident::FirstFailure(where, evidence);
  REXLOG_ERROR("[narutorise] {}: the engine could not be released: {}", where, evidence);
  rex::FlushLogging();
  std::_Exit(88);
}

// The reason Initialize refuses a render mode, or null.
inline const char* RenderModeError(const RenderMode& mode) {
  const ResolutionScale& scale = mode.scale;
  if (!(scale.x >= 1.0f && scale.x <= 4.0f && scale.y >= 1.0f && scale.y <= 4.0f)) return "native-resolution-scale";
  if (mode.msaa_samples && mode.msaa_samples != 2 && mode.msaa_samples != 4 && mode.msaa_samples != 8)
    return "native-msaa-samples";
  return nullptr;
}

// The MSAA sample count of a render mode lowered to one the device supports
// for every surface format (supported(samples)); 0 keeps the title's counts.
template <typename Supported>
uint32_t SupportedSamples(uint32_t samples, Supported supported) {
  while (samples > 2 && !supported(samples)) samples /= 2;
  if (samples == 2 && !supported(2)) samples = 0;
  return samples;
}

// A number for each initialized engine, unique in the process and never 0;
// 0 once exhausted.
inline uint64_t AllocateEngineEpoch() {
  static std::atomic<uint64_t> next{1};
  uint64_t epoch = next.load(std::memory_order_relaxed);
  while (epoch != 0 && epoch != UINT64_MAX)
    if (next.compare_exchange_weak(epoch, epoch + 1, std::memory_order_relaxed)) return epoch;
  return 0;
}

// The evidence of a frontend generation verified ready on the engine's owner
// thread: nothing is admitted, submitted or completed for the guest yet.
inline std::string FrontendReadyEvidence(uint64_t generation, uint64_t epoch, uint32_t owner_thread) {
  return fmt::format("{{\"session_generation\":{},\"engine_epoch\":{},\"owner_thread\":{},"
      "\"invalidated_categories\":[\"shader\",\"resources\",\"constants\",\"raster\",\"blend\","
      "\"depth-stencil\",\"viewport\",\"targets\"],\"interpolation\":\"centroid\","
      "\"generated_parameter_slot\":0,\"flat_components\":0,\"pending_work\":0,"
      "\"callback_pending\":false,\"interrupt_armed\":false,\"scratch_result_valid\":false,"
      "\"configuration_blockers\":[\"shader-flow\",\"hardware-sequencer\",\"clip-enhancement\","
      "\"performance-context\"],\"operations_admitted\":false,\"guest_completion\":false,"
      "\"gpu_submission\":false,\"debug_messages\":0}}", generation, epoch, owner_thread);
}

inline std::string FrontendReleasedEvidence(uint64_t generation) {
  return fmt::format("{{\"session_generation\":{},\"frontend_released\":true,"
                     "\"pending_work\":0,\"guest_completion\":false}}", generation);
}

}  // namespace gpu
