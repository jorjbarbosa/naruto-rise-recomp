#pragma once

// The native XMA in the runtime (xma_runtime.cpp): the audio system that owns
// the XMA registers and contexts, and the XMA* exports by gate slot.
#include <memory>
#include <string>
#include <string_view>
#include <rex/ppc/context.h>
#include <rex/system/interfaces/audio.h>

namespace narutorise::audio::xma_runtime {
// The gate's slot of an XMA export, or -1 (kernel_xma.cpp).
constexpr int Slot(std::string_view name) {
  constexpr std::string_view names[] = {
      "__imp__XMACreateContext",          "__imp__XMAReleaseContext",
      "__imp__XMAInitializeContext",      "__imp__XMASetLoopData",
      "__imp__XMAGetInputBufferReadOffset", "__imp__XMASetInputBufferReadOffset",
      "__imp__XMASetInputBuffer0",        "__imp__XMAIsInputBuffer0Valid",
      "__imp__XMASetInputBuffer0Valid",   "__imp__XMASetInputBuffer1",
      "__imp__XMAIsInputBuffer1Valid",    "__imp__XMASetInputBuffer1Valid",
      "__imp__XMAIsOutputBufferValid",    "__imp__XMASetOutputBufferValid",
      "__imp__XMAGetOutputBufferReadOffset", "__imp__XMASetOutputBufferReadOffset",
      "__imp__XMAGetOutputBufferWriteOffset", "__imp__XMAGetPacketMetadata",
      "__imp__XMAEnableContext",          "__imp__XMADisableContext",
      "__imp__XMABlockWhileInUse"};
  for (int i = 0; i < int(std::size(names)); ++i)
    if (names[i] == name) return i;
  return -1;
}
void Call(int slot, PPCContext& ctx, uint8_t* base);
std::unique_ptr<rex::system::IAudioSystem> CreateSystem();
std::string Json();
}  // namespace narutorise::audio::xma_runtime
