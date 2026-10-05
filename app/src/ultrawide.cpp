// narutorise - ultrawide anamórfico fiel ao Hells Gate Recomp.
// Patch da constante de projeção (igual ao Xenia Canary) + presenter anamórfico.

#include "ultrawide.h"

#include <cmath>
#include <cstdint>
#include <bit>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>

REXCVAR_DEFINE_DOUBLE(ultrawide_target_aspect, 0.0, "narutorise",
                      "Target aspect ratio for ultrawide (0=disabled, 1.7778=16:9, "
                      "2.3889=21:9, 3.5556=32:9)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

constexpr float kNativeAspect = 16.0f / 9.0f;
// Constante 9/16 (0.5625) usada pelo patch 21:9 do Xenia Canary para Naruto.
// Substituída pelo recíproco do aspect alvo (1/2.3889 = 0.4186 para 3440x1440).
constexpr uint32_t kProjectionAspectFactorAddress = 0x826F5108;

float g_current_projection_factor = 9.0f / 16.0f;

uint32_t LoadGuestU32(uint8_t* base, uint32_t address) {
  uint32_t raw = 0;
  std::memcpy(&raw, base + address, sizeof(raw));
  return std::byteswap(raw);
}

void StoreGuestU32(uint8_t* base, uint32_t address, uint32_t value) {
  uint32_t raw = std::byteswap(value);
  std::memcpy(base + address, &raw, sizeof(raw));
}

}  // namespace

void UpdateUltrawideProjectionFactor(uint8_t* base) {
  if (!base) {
    return;
  }

  double target_aspect = REXCVAR_GET(ultrawide_target_aspect);
  if (!std::isfinite(target_aspect) || target_aspect < 0.0 || target_aspect > 8.0) {
    REXLOG_WARN("[narutorise] ultrawide: invalid target aspect {:.4f}", target_aspect);
    return;
  }

  // Se ultrawide desligado ou <= 16:9, restaura o fator original 9/16.
  const float target_factor = (target_aspect <= kNativeAspect + 0.01)
                                  ? (9.0f / 16.0f)
                                  : (1.0f / static_cast<float>(target_aspect));

  constexpr float kExpectedFactor = 9.0f / 16.0f;
  const uint32_t expected_bits = std::bit_cast<uint32_t>(kExpectedFactor);
  const uint32_t current_factor_bits = std::bit_cast<uint32_t>(g_current_projection_factor);
  const uint32_t current_bits = LoadGuestU32(base, kProjectionAspectFactorAddress);

  if (current_bits != expected_bits && current_bits != current_factor_bits) {
    REXLOG_ERROR(
        "[narutorise] ultrawide: projection constant mismatch at {:08X} (expected {:08X} or {:08X}, found {:08X})",
        kProjectionAspectFactorAddress, expected_bits, current_factor_bits, current_bits);
    return;
  }

  if (current_bits == std::bit_cast<uint32_t>(target_factor)) {
    return;
  }

  void* patch_address = base + kProjectionAspectFactorAddress;
  rex::memory::PageAccess old_access{};
  if (!rex::memory::Protect(patch_address, sizeof(uint32_t),
                            rex::memory::PageAccess::kReadWrite, &old_access)) {
    REXLOG_ERROR("[narutorise] ultrawide: cannot make projection page writable");
    return;
  }
  StoreGuestU32(base, kProjectionAspectFactorAddress,
                std::bit_cast<uint32_t>(target_factor));
  if (!rex::memory::Protect(patch_address, sizeof(uint32_t), old_access)) {
    REXLOG_WARN("[narutorise] ultrawide: cannot restore projection page protection");
  }
  g_current_projection_factor = target_factor;
  REXLOG_INFO("[narutorise] ultrawide: projection factor updated to {:.6f} (target aspect {:.4f})",
              target_factor, target_aspect);
}

// O presenter é informado do aspecto alvo em IssueSwap (SDK), não aqui:
// alterar video_mode_* ou present_letterbox mudaria o que o guest enxerga.
void ApplyUltrawidePatch(uint8_t* base) {
  UpdateUltrawideProjectionFactor(base);
}