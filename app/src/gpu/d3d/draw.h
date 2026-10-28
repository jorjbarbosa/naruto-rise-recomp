#pragma once

// Draw layer of the D3D boundary (draw.cpp): the BeginVertices quad, the own
// draw, the discard scope of the originals and the counters of the frame.
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <rex/ppc/context.h>
#include "gpu/common/guest_memory.h"

namespace rex::memory { class Memory; }

namespace gpu {
constexpr uint32_t kDiscardBytes = 0x10000, kDiscardLimit = 0xC000, kVertexBytes = 0x10000, kPatchBytes = 0x10000;
struct PendingDraw {
  uint32_t vertices, primitive, count, stride, shader12684, shader12688, target, depth, lr, thread;
  uint64_t call;
};
struct InlineBuild;  // The quad request built at the commit (below OwnDraw).
struct InlineDraw {
  std::mutex mutex;
  rex::memory::Memory* memory = nullptr;
  uint32_t discard = 0, vertices = 0, vertex_cursor = 0;
  uint32_t patch = 0;  // Own copy of the VS microcode for the fetch patch.
  std::atomic<uint32_t> scope_thread{0};  // Guest thread inside the original commit.
  uint32_t scope_vertices = 0;
  // Written under the mutex; read without it first, so a draw with no quad
  // pending does not take the lock.
  std::atomic<bool> has_pending{false};
  PendingDraw pending{};
  std::shared_ptr<InlineBuild> build;  // Null when the quad has no own draw.
  uint64_t calls = 0, taken = 0;
};
extern InlineDraw inline_draw;
// BEFORE 821024A8/LR 82108330 of the last arrival on this thread. The
// producer tail-calls DrawIndexedVertices, which takes it (drawn) as its origin.
struct ProducerBefore {
  uint64_t call = 0, arrival = 0, thread_call = 0;
  uint32_t r3 = 0, r4 = 0, r5 = 0, r1 = 0;
  bool drawn = true;
};
extern thread_local ProducerBefore producer_before;
extern std::atomic<uint32_t> discard_thread;  // Guest thread inside RunOverDiscard.
extern std::atomic<uint64_t> own_draws, start_draws, start_first_swap, unpredicated_commits;
extern std::atomic<uint64_t> scene_items, scene_flushes, kicks, producer_arrivals, producer_calls;
// The pending BeginVertices quad is drawn before the next own operation
// of its thread; ctx is null at the stop.
void TakePendingDraw(PPCContext* ctx, uint8_t* base, const char* by);
// Called by StopInOrder before the engine is released.
void ReleaseInlineDraw();
uint64_t InlineDrawCalls();
uint32_t RunOverDiscard(PPCContext& ctx, uint8_t* base, uint32_t dev, GuestFunction fn, std::string_view name,
                        bool clear_predication = false);
// The draws are translated on the translation stage of the
// dispatcher, on by default (REX_NATIVE_DEFERRED_TRANSLATION=0 translates
// inline on the title thread); fixed per session, and off with the
// diagnostics that read the device and the context after the translation
// (the records outside the measurement, the START variation and policy, the
// frame trace).
bool DeferredTranslation();
// The sync interrupt requests the leaf 8212F7E0 captured, taken by the own Swap.
struct LeafInterrupt { uint32_t routine, argument, cpu_mask, lr; };
extern std::atomic<uint64_t> leaf_interrupt_count;
std::vector<LeafInterrupt> TakeLeafInterrupts();
}  // namespace gpu
