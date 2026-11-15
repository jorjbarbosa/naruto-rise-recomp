#pragma once

#include <cstdint>

// The XMA decoder hardware of the APU. Contexts are 64-byte blocks in physical memory, laid out as
// the hardware reads them; the title fills them through the XMA* imports or writes them itself
// (XMAPlayback, under the modify lock). One APU thread decodes every enabled context: whole 2048-byte
// input packets, from input buffer 0 and 1 in turn, into the 256-byte blocks of the output ring, as
// 16-bit big-endian PCM. It does not run ahead of the title: each audio frame it writes only the
// subframes the context asks for, as the hardware does (DARE drops an output ring it finds full).
//
// All functions are thread-safe. They take the hardware lock, which the APU thread holds while it
// services the contexts, so a context changed through them is never half-processed.
namespace apu
{
    constexpr uint32_t kXmaContextSize = 64;
    // Context index of an address that is not a context.
    constexpr uint32_t kNoXmaContext = ~0u;

    // XMA_CONTEXT_INIT (XMAInitializeContext): buffers as guest addresses, as the title passes them.
    struct XmaContextInit
    {
        uint32_t inputBuffer0 = 0;
        uint32_t inputBuffer0PacketCount = 0;
        uint32_t inputBuffer1 = 0;
        uint32_t inputBuffer1PacketCount = 0;
        uint32_t inputBufferReadOffset = 0; // bits
        uint32_t outputBuffer = 0;
        uint32_t outputBufferBlockCount = 0;
        uint32_t workBuffer = 0;
        uint32_t subframeDecodeCount = 0;
        uint32_t stereo = 0;     // channels - 1
        uint32_t sampleRate = 0; // 0 = 24 kHz, 1 = 32 kHz, 2 = 44.1 kHz, 3 = 48 kHz
        uint32_t loopStart = 0;  // bits
        uint32_t loopEnd = 0;
        uint8_t loopCount = 0;
        uint8_t loopSubframeEnd = 0;
        uint8_t loopSubframeSkip = 0;
    };

    // Allocates the contexts in physical memory and publishes their base in the APU register the XMA
    // library reads (XAudioInitialize), then starts the APU thread. Once, at startup, before the
    // title runs.
    void InitializeXma();

    // Marks the start of an audio frame (256 samples at 48 kHz); the APU makes one pass over the
    // enabled contexts per frame, decoding at most their subframe decode count each. Called by the
    // audio render driver, so decoding follows the output device's clock.
    void SignalAudioFrame();

    // A free context, cleared and disabled, or 0 when all are taken.
    uint32_t AllocateContext();
    // Disables the context and frees it. Ignores addresses that are not allocated contexts.
    void ReleaseContext(uint32_t context);
    // The hardware index of a context (its offset in the context array / 64), or kNoXmaContext.
    uint32_t ContextIndex(uint32_t context);

    // Writes the context from init (disabled, both input buffers and the output buffer not valid)
    // and clears the decoder state.
    void InitializeContext(uint32_t context, const XmaContextInit& init);

    // The per-context bits of the APU registers. Enable (kick) lets the APU service the context;
    // disable (lock) stops it, and returns once the APU no longer touches the context; clear
    // forgets the decoder state (the next packet starts a new stream).
    void EnableContext(uint32_t index);
    void DisableContext(uint32_t index);
    void ClearContext(uint32_t index);

    // Context fields, read and written under the hardware lock. buffer is 0 or 1.
    bool IsInputBufferValid(uint32_t context, int buffer);
    void SetInputBufferValid(uint32_t context, int buffer);
    bool IsOutputBufferValid(uint32_t context);
    void SetOutputBufferValid(uint32_t context);
    uint32_t GetOutputBufferReadOffset(uint32_t context);
    void SetOutputBufferReadOffset(uint32_t context, uint32_t offset);
    uint32_t GetOutputBufferWriteOffset(uint32_t context);
}
