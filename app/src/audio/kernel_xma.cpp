// The SDK's XMA exports, compiled into the executable behind the gate of the
// native audio. With the native backend, each call goes to xma_runtime.cpp;
// with the SDK's, to the SDK. Either way the measurement of stage
// 2a counts the calls, notes the profile of each context initialized and
// follows the output of DARE's streams.
#include <string_view>
#include <rex/hook.h>
#include "audio/audio.h"
#include "audio/measure.h"
#include "audio/xma_runtime.h"

#undef REX_HOOK
#define REX_HOOK(name, function)                                                                   \
  extern "C" REX_FUNC(name) {                                                                      \
    static const int measure_slot = ::narutorise::audio::measure::RegisterImport(#name);          \
    ::narutorise::audio::measure::CountImport(measure_slot);                                       \
    const uint32_t measure_context = ctx.r3.u32;                                                   \
    if (std::string_view(#name) == "__imp__XMAInitializeContext")                                  \
      ::narutorise::audio::measure::NoteInit(ctx.r4.u32);                                           \
    [&] {                                                                                          \
      if constexpr (constexpr int slot = ::narutorise::audio::xma_runtime::Slot(#name); slot >= 0) { \
        if (::narutorise::audio::Native()) {                                                       \
          ::narutorise::audio::xma_runtime::Call(slot, ctx, base);                                 \
          return;                                                                                  \
        }                                                                                          \
      }                                                                                            \
      rex::ppc::HostToGuestFunction<function>(ctx, base);                                          \
    }();                                                                                           \
    ::narutorise::audio::measure::AfterImport(#name, measure_context, ctx.r3.u32);                \
  }

#include "../../sdk/src/kernel/xboxkrnl/xboxkrnl_audio_xma.cpp"
