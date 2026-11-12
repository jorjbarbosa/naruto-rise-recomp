#pragma once

// The gate of the audio exports (kernel_audio.cpp): the SDK's export unit is
// compiled into the executable with REX_HOOK redefined, so each title call of
// these imports reaches the native audio when it is in use, and the SDK's
// implementation otherwise. Exports outside the table keep the SDK's.
//
// Like the renderer's (gpu/host/kernel_exports.h), this binds the title's
// direct calls. A dynamic lookup of a kernel export (KernelModule's
// GetProcAddressByOrdinal) would still find the SDK's: the title imports
// these by ordinal at load, and the boot test checks that the SDK's audio
// receives no client.
#include <cstdint>
#include <string_view>
#include <rex/hook.h>
#include <rex/ppc/context.h>

namespace narutorise::audio {
bool Native();
}  // namespace narutorise::audio

namespace narutorise::audio::kernel {
constexpr int Slot(std::string_view name) {
  if (name == "__imp__XAudioRegisterRenderDriverClient") return 0;
  if (name == "__imp__XAudioUnregisterRenderDriverClient") return 1;
  if (name == "__imp__XAudioSubmitRenderDriverFrame") return 2;
  if (name == "__imp__XAudioGetVoiceCategoryVolume") return 3;
  if (name == "__imp__XAudioGetVoiceCategoryVolumeChangeMask") return 4;
  return -1;
}
// The native export of a slot (runtime.cpp).
template <int slot>
void Call(PPCContext& ctx, uint8_t* base);
template <> void Call<0>(PPCContext& ctx, uint8_t* base);
template <> void Call<1>(PPCContext& ctx, uint8_t* base);
template <> void Call<2>(PPCContext& ctx, uint8_t* base);
template <> void Call<3>(PPCContext& ctx, uint8_t* base);
template <> void Call<4>(PPCContext& ctx, uint8_t* base);
}  // namespace narutorise::audio::kernel

#undef REX_HOOK
#define REX_HOOK(name, function)                                                               \
  extern "C" REX_FUNC(name) {                                                                  \
    if constexpr (constexpr int slot = ::narutorise::audio::kernel::Slot(#name); slot >= 0) { \
      if (::narutorise::audio::Native()) {                                                     \
        ::narutorise::audio::kernel::Call<slot>(ctx, base);                                    \
        return;                                                                                \
      }                                                                                        \
    }                                                                                          \
    rex::ppc::HostToGuestFunction<function>(ctx, base);                                        \
  }
