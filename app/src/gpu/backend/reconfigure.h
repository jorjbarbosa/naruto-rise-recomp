#pragma once
#include "gpu/backend/contracts.h"
#include <cmath>

namespace gpu {
inline bool ValidLiveModes(const RenderMode& old, const RenderMode& render, const PresentMode& present) {
  return render.api == old.api && render.device == old.device &&
      std::isfinite(render.scale.x) && std::isfinite(render.scale.y) &&
      render.scale.x >= 1 && render.scale.x <= 4 && render.scale.y >= 1 && render.scale.y <= 4 &&
      (render.msaa_samples == 0 || render.msaa_samples == 2 || render.msaa_samples == 4 || render.msaa_samples == 8) &&
      present.buffers >= 2 && present.buffers <= 3 && present.max_frame_latency >= 1 && present.max_frame_latency <= 3 &&
      std::isfinite(present.brightness) && present.brightness >= 0 && present.brightness <= 1 &&
      (present.effect == PresentEffect::kBilinear || present.effect == PresentEffect::kCas || present.effect == PresentEffect::kFsr) &&
      (present.post_effect == SwapPostEffect::kNone || present.post_effect == SwapPostEffect::kFxaa || present.post_effect == SwapPostEffect::kFxaaExtreme) &&
      std::isfinite(present.cas_sharpness) && present.cas_sharpness >= 0 && present.cas_sharpness <= 1 &&
      std::isfinite(present.fsr_sharpness_reduction) && present.fsr_sharpness_reduction >= 0 &&
      present.fsr_sharpness_reduction <= 2 && present.fsr_passes >= 1 && present.fsr_passes <= 4;
}
}  // namespace gpu
