// XMA* imports: the kernel side of the XMA decoder hardware (src/apu/xma.h).

#include "apu/xma.h"
#include "kernel/kernel.h"

using namespace kernel;

namespace
{
    // Hardware index of a context the title passes, or kNoXmaContext (logged once).
    uint32_t IndexOf(const char* function, uint32_t context)
    {
        uint32_t index = apu::ContextIndex(context);
        if (index == apu::kNoXmaContext)
            LOG_ONCE("error: %s: %08X is not an XMA context", function, context);
        return index;
    }
}

// XMACreateContext(context*)
GUEST_FUNC(XMACreateContext)
{
    uint32_t context = apu::AllocateContext();
    if (context == 0)
    {
        LOG_ONCE("error: XMACreateContext: no free XMA context");
        ctx.r3.u64 = X_STATUS_NO_MEMORY;
        return;
    }
    mem::Store32(ctx.r3.u32, context);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// XMAReleaseContext(context)
GUEST_FUNC(XMAReleaseContext)
{
    apu::ReleaseContext(ctx.r3.u32);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// XMAInitializeContext(context, init*): XMA_CONTEXT_INIT, 12 words then the loop data (two words
// and three bytes).
GUEST_FUNC(XMAInitializeContext)
{
    uint32_t context = ctx.r3.u32;
    uint32_t init = ctx.r4.u32;
    if (IndexOf("XMAInitializeContext", context) == apu::kNoXmaContext || init == 0)
    {
        ctx.r3.u64 = X_STATUS_INVALID_PARAMETER;
        return;
    }
    apu::XmaContextInit fields;
    fields.inputBuffer0 = mem::Load32(init + 0x00);
    fields.inputBuffer0PacketCount = mem::Load32(init + 0x04);
    fields.inputBuffer1 = mem::Load32(init + 0x08);
    fields.inputBuffer1PacketCount = mem::Load32(init + 0x0C);
    fields.inputBufferReadOffset = mem::Load32(init + 0x10);
    fields.outputBuffer = mem::Load32(init + 0x14);
    fields.outputBufferBlockCount = mem::Load32(init + 0x18);
    fields.workBuffer = mem::Load32(init + 0x1C);
    fields.subframeDecodeCount = mem::Load32(init + 0x20);
    fields.stereo = mem::Load32(init + 0x24);
    fields.sampleRate = mem::Load32(init + 0x28);
    fields.loopStart = mem::Load32(init + 0x2C);
    fields.loopEnd = mem::Load32(init + 0x30);
    fields.loopCount = mem::Load8(init + 0x34);
    fields.loopSubframeEnd = mem::Load8(init + 0x35);
    fields.loopSubframeSkip = mem::Load8(init + 0x36);
    apu::InitializeContext(context, fields);
    TRACE("XMAInitializeContext(%08X): input %08X/%u %08X/%u, output %08X x%u, rate %u, stereo %u",
        context, fields.inputBuffer0, fields.inputBuffer0PacketCount, fields.inputBuffer1,
        fields.inputBuffer1PacketCount, fields.outputBuffer, fields.outputBufferBlockCount, fields.sampleRate,
        fields.stereo);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// XMAEnableContext(context)
GUEST_FUNC(XMAEnableContext)
{
    uint32_t index = IndexOf("XMAEnableContext", ctx.r3.u32);
    if (index != apu::kNoXmaContext)
        apu::EnableContext(index);
    ctx.r3.u64 = index != apu::kNoXmaContext ? X_STATUS_SUCCESS : X_STATUS_INVALID_PARAMETER;
}

// XMADisableContext(context, wait): the APU is always done with the context when it returns, so
// wait makes no difference.
GUEST_FUNC(XMADisableContext)
{
    uint32_t index = IndexOf("XMADisableContext", ctx.r3.u32);
    if (index != apu::kNoXmaContext)
        apu::DisableContext(index);
    ctx.r3.u64 = index != apu::kNoXmaContext ? X_STATUS_SUCCESS : X_STATUS_INVALID_PARAMETER;
}

GUEST_FUNC(XMAIsInputBuffer0Valid) { ctx.r3.u64 = apu::IsInputBufferValid(ctx.r3.u32, 0) ? 1 : 0; }
GUEST_FUNC(XMAIsInputBuffer1Valid) { ctx.r3.u64 = apu::IsInputBufferValid(ctx.r3.u32, 1) ? 1 : 0; }
GUEST_FUNC(XMASetInputBuffer0Valid) { apu::SetInputBufferValid(ctx.r3.u32, 0); }
GUEST_FUNC(XMASetInputBuffer1Valid) { apu::SetInputBufferValid(ctx.r3.u32, 1); }
GUEST_FUNC(XMAIsOutputBufferValid) { ctx.r3.u64 = apu::IsOutputBufferValid(ctx.r3.u32) ? 1 : 0; }
GUEST_FUNC(XMASetOutputBufferValid) { apu::SetOutputBufferValid(ctx.r3.u32); }
GUEST_FUNC(XMAGetOutputBufferReadOffset) { ctx.r3.u64 = apu::GetOutputBufferReadOffset(ctx.r3.u32); }
GUEST_FUNC(XMAGetOutputBufferWriteOffset) { ctx.r3.u64 = apu::GetOutputBufferWriteOffset(ctx.r3.u32); }

// XMASetOutputBufferReadOffset(context, offset)
GUEST_FUNC(XMASetOutputBufferReadOffset) { apu::SetOutputBufferReadOffset(ctx.r3.u32, ctx.r4.u32); }
