// The XMA decoder hardware: contexts in physical memory, serviced by one APU thread.

#include "apu/xma.h"

#include "apu/xma_decoder.h"
#include "log.h"
#include "memory.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace apu
{
    namespace
    {
        constexpr uint32_t kContextCount = 320;
        // APU register with the physical address of the context array, read byte-reversed (lwbrx)
        // by XAudioInitialize into the base XMAPlayback derives context indices from.
        constexpr uint32_t kContextArrayRegister = 0x7FEA1800;
        // Physical memory is the physical heap at 0xA0000000 (mem::Heap::Physical).
        constexpr uint32_t kPhysicalBase = 0xA0000000;
        constexpr uint32_t kPhysicalMask = 0x1FFFFFFF;
        constexpr uint32_t kBlockSize = 256;
        constexpr size_t kBlockSamples = kBlockSize / sizeof(int16_t);
        constexpr uint32_t kPacketBits = kXmaPacketSize * 8;
        // The packet header (32 bits) comes before the first frame of a packet.
        constexpr uint32_t kPacketHeaderBits = 32;
        // Loop count that never runs out.
        constexpr uint32_t kInfiniteLoops = 255;
        // The APU makes one pass over the contexts per audio frame (256 samples at 48 kHz). The
        // audio driver signals its frames, which keeps the decoders on the output device's clock;
        // without them for a while, the APU runs on its own clock.
        constexpr auto kFramePeriod = std::chrono::microseconds(5333);
        constexpr auto kDriverTimeout = std::chrono::milliseconds(100);
        // Frames that piled up while the APU was busy are caught up, but only this many at once.
        constexpr uint32_t kMaxPassesPerWake = 4;
        // Packets one pass may decode for a context. One packet holds far more than a pass writes;
        // the limit keeps a looping stream whose packets give no samples (a decoder that cannot
        // open, bad data) from spinning under the hardware lock.
        constexpr uint32_t kMaxPacketsPerPass = 4;

        // Fields of the hardware context, one 32-bit big-endian word each, LSB-first bits.
        struct Field
        {
            uint32_t word;
            uint32_t shift;
            uint32_t bits;
        };
        constexpr Field kInput0PacketCount{ 0, 0, 12 };
        constexpr Field kLoopCount{ 0, 12, 8 };
        constexpr Field kInput0Valid{ 0, 20, 1 };
        constexpr Field kInput1Valid{ 0, 21, 1 };
        constexpr Field kOutputBlockCount{ 0, 22, 5 };
        constexpr Field kOutputWriteOffset{ 0, 27, 5 };
        constexpr Field kInput1PacketCount{ 1, 0, 12 };
        constexpr Field kLoopSubframeEnd{ 1, 14, 3 };
        constexpr Field kLoopSubframeSkip{ 1, 17, 3 };
        constexpr Field kSubframeDecodeCount{ 1, 20, 4 };
        constexpr Field kSampleRate{ 1, 27, 2 };
        constexpr Field kStereo{ 1, 29, 1 };
        constexpr Field kOutputValid{ 1, 31, 1 };
        constexpr Field kInputReadOffset{ 2, 0, 26 };
        constexpr Field kLoopStart{ 3, 0, 26 };
        constexpr Field kLoopEnd{ 4, 0, 26 };
        constexpr Field kCurrentBuffer{ 4, 31, 1 };
        constexpr uint32_t kInput0Pointer = 5;
        constexpr uint32_t kInput1Pointer = 6;
        constexpr uint32_t kOutputPointer = 7;
        constexpr uint32_t kWorkPointer = 8;
        constexpr Field kOutputReadOffset{ 9, 0, 5 };

        constexpr uint32_t kSampleRates[] = { 24000, 32000, 44100, 48000 };

        uint32_t Mask(const Field& f)
        {
            return f.bits == 32 ? ~0u : ((1u << f.bits) - 1) << f.shift;
        }

        // A context's words, loaded once and stored back by the APU thread.
        struct Words
        {
            uint32_t w[10];

            uint32_t Get(const Field& f) const { return (w[f.word] & Mask(f)) >> f.shift; }
            void Set(const Field& f, uint32_t value) { w[f.word] = (w[f.word] & ~Mask(f)) | ((value << f.shift) & Mask(f)); }
        };

        uint32_t LoadField(uint32_t context, const Field& f)
        {
            return (mem::Load32(context + f.word * 4) & Mask(f)) >> f.shift;
        }

        void StoreField(uint32_t context, const Field& f, uint32_t value)
        {
            uint32_t address = context + f.word * 4;
            mem::Store32(address, (mem::Load32(address) & ~Mask(f)) | ((value << f.shift) & Mask(f)));
        }

        uint32_t GuestPhysical(uint32_t physical)
        {
            return kPhysicalBase + (physical & kPhysicalMask);
        }

        // How the input moves on once a decoded packet has been written out.
        struct InputAdvance
        {
            bool pending = false;
            uint32_t buffer = 0;        // the input buffer the packet came from
            bool consumed = false;      // it was the buffer's last packet: hand the buffer back
            bool loop = false;          // the packet held the loop end: one loop is used
            uint32_t readOffset = 0;    // read offset after the packet
        };

        struct Context
        {
            bool allocated = false;
            bool enabled = false;
            XmaDecoder decoder;
            // Decoded samples (host order, interleaved) not yet written to the output ring. The
            // input moves past their packet only once they are out, as the hardware consumes input
            // only as fast as it fills the ring.
            std::vector<int16_t> pending;
            size_t pendingRead = 0;
            InputAdvance advance;
            // Read and write offsets as the APU last left them: a change means the title moved the
            // stream (a new sound, a flush), so the decoder state and pending samples are dropped.
            uint32_t lastReadOffset = ~0u;
            uint32_t lastWriteOffset = ~0u;
        };

        // Frame signals, under their own lock so the audio driver never waits for a decode.
        std::mutex g_frameMutex;
        std::condition_variable g_frameSignal;
        uint32_t g_frames = 0; // audio frames signalled and not yet passed over

        // The hardware lock: guards the contexts and every FFmpeg call.
        std::mutex g_mutex;
        Context g_contexts[kContextCount];
        uint32_t g_array = 0;

        Context* Find(uint32_t context, uint32_t* index = nullptr)
        {
            if (context < g_array || context >= g_array + kContextCount * kXmaContextSize || (context - g_array) % kXmaContextSize != 0)
                return nullptr;
            uint32_t i = (context - g_array) / kXmaContextSize;
            if (index != nullptr)
                *index = i;
            return g_contexts[i].allocated ? &g_contexts[i] : nullptr;
        }

        uint32_t Address(uint32_t index)
        {
            return g_array + index * kXmaContextSize;
        }

        void ResetStream(Context& c)
        {
            c.decoder.Reset();
            c.pending.clear();
            c.pendingRead = 0;
            c.advance = {};
        }

        const Field& InputValid(uint32_t buffer)
        {
            return buffer == 0 ? kInput0Valid : kInput1Valid;
        }

        // Moves the input past the packet whose samples have all been written out: a consumed
        // buffer loses its valid bit and the other one becomes current.
        void ApplyAdvance(Context& c, Words& words)
        {
            if (!c.advance.pending)
                return;
            uint32_t loops = words.Get(kLoopCount);
            if (c.advance.loop && loops != 0 && loops != kInfiniteLoops)
                words.Set(kLoopCount, loops - 1);
            if (c.advance.consumed)
            {
                words.Set(InputValid(c.advance.buffer), 0);
                words.Set(kCurrentBuffer, c.advance.buffer ^ 1);
            }
            words.Set(kInputReadOffset, c.advance.readOffset);
            c.advance = {};
        }

        // Decodes the packet at the read offset of the current input buffer into c.pending and
        // records in c.advance where the input goes next. False when no input is valid.
        bool DecodeNextPacket(Context& c, Words& words)
        {
            for (int attempt = 0; attempt < 2; attempt++)
            {
                uint32_t current = words.Get(kCurrentBuffer);
                if (words.Get(InputValid(current)) == 0)
                {
                    if (words.Get(InputValid(current ^ 1)) == 0)
                        return false;
                    words.Set(kCurrentBuffer, current ^ 1);
                    words.Set(kInputReadOffset, kPacketHeaderBits);
                    continue;
                }
                uint32_t packetCount = words.Get(current == 0 ? kInput0PacketCount : kInput1PacketCount);
                uint32_t buffer = words.w[current == 0 ? kInput0Pointer : kInput1Pointer];
                uint32_t packet = words.Get(kInputReadOffset) / kPacketBits;
                if (packet >= packetCount)
                {
                    // Nothing left to decode in this buffer: hand it back now.
                    words.Set(InputValid(current), 0);
                    words.Set(kCurrentBuffer, current ^ 1);
                    words.Set(kInputReadOffset, kPacketHeaderBits);
                    continue;
                }
                uint32_t address = GuestPhysical(buffer) + packet * kXmaPacketSize;
                uint32_t channels = words.Get(kStereo) + 1;
                c.decoder.DecodePacket(mem::Ptr(address), kSampleRates[words.Get(kSampleRate)], channels, c.pending);
                c.advance.pending = true;
                c.advance.buffer = current;
                // A loop goes back to its start after the packet with its end. The decoder keeps
                // running across the jump, as the hardware does; the frames of that packet past the
                // loop end still play (packets are the unit here).
                if (words.Get(kLoopCount) != 0 && words.Get(kLoopEnd) / kPacketBits == packet)
                {
                    c.advance.loop = true;
                    c.advance.readOffset = words.Get(kLoopStart);
                }
                else if (packet + 1 < packetCount)
                {
                    c.advance.readOffset = (packet + 1) * kPacketBits + kPacketHeaderBits;
                }
                else
                {
                    c.advance.consumed = true;
                    c.advance.readOffset = kPacketHeaderBits;
                }
                return true;
            }
            return false;
        }

        // One pass over a context: writes up to its subframe decode count of subframes (128
        // samples per channel, one block per channel) into the free blocks of the output ring,
        // decoding packets as needed. The ring is full when the write offset reaches the read
        // offset; the APU then clears the output valid bit until the title frees blocks and sets
        // it again.
        void Service(Context& c, uint32_t context)
        {
            Words words;
            for (uint32_t i = 0; i < 10; i++)
                words.w[i] = mem::Load32(context + i * 4);
            if (words.Get(kOutputValid) == 0)
                return;
            const Words loaded = words;

            uint32_t readOffset = words.Get(kInputReadOffset);
            uint32_t writeOffset = words.Get(kOutputWriteOffset);
            if (readOffset != c.lastReadOffset || writeOffset != c.lastWriteOffset)
                ResetStream(c);

            uint32_t blocks = words.Get(kOutputBlockCount);
            if (blocks == 0)
                blocks = 32;
            uint32_t outputRead = words.Get(kOutputReadOffset) % blocks;
            writeOffset %= blocks;
            uint32_t free = (outputRead + blocks - writeOffset) % blocks;
            if (free == 0)
                free = blocks; // valid with equal offsets: the whole ring is free
            uint32_t output = GuestPhysical(words.w[kOutputPointer]);

            // Whole subframes only, as many as the context asks for in a pass.
            uint32_t subframeBlocks = words.Get(kStereo) + 1;
            uint32_t budget = std::max<uint32_t>(words.Get(kSubframeDecodeCount), 1) * subframeBlocks;
            budget = std::min(budget, free - free % subframeBlocks);

            uint32_t packets = 0;
            while (budget > 0)
            {
                if (c.pending.size() - c.pendingRead >= kBlockSamples)
                {
                    uint32_t block = output + writeOffset * kBlockSize;
                    for (size_t i = 0; i < kBlockSamples; i++)
                        mem::Store16(block + uint32_t(i * 2), uint16_t(c.pending[c.pendingRead + i]));
                    c.pendingRead += kBlockSamples;
                    writeOffset = (writeOffset + 1) % blocks;
                    free--;
                    budget--;
                    continue;
                }
                // The packet is out but for a partial block, which waits for the next one.
                c.pending.erase(c.pending.begin(), c.pending.begin() + ptrdiff_t(c.pendingRead));
                c.pendingRead = 0;
                ApplyAdvance(c, words);
                if (packets++ == kMaxPacketsPerPass || !DecodeNextPacket(c, words))
                    break;
            }
            if (free == 0)
                words.Set(kOutputValid, 0);
            words.Set(kOutputWriteOffset, writeOffset);

            // Only the bits the APU changed: the title may have set others (a valid bit) meanwhile.
            for (uint32_t i : { 0u, 1u, 2u, 4u })
            {
                uint32_t changed = words.w[i] ^ loaded.w[i];
                if (changed != 0)
                {
                    uint32_t address = context + i * 4;
                    mem::Store32(address, (mem::Load32(address) & ~changed) | (words.w[i] & changed));
                }
            }
            c.lastReadOffset = words.Get(kInputReadOffset);
            c.lastWriteOffset = writeOffset;
        }

        // One pass per audio frame: the frames signalled by the audio driver, or, while it sends
        // none, the APU's own clock.
        void ApuThread()
        {
            auto lastSignal = std::chrono::steady_clock::time_point{};
            for (;;)
            {
                uint32_t passes = 0;
                {
                    std::unique_lock frameLock(g_frameMutex);
                    bool signalled = g_frameSignal.wait_for(frameLock, kFramePeriod, [] { return g_frames != 0; });
                    auto now = std::chrono::steady_clock::now();
                    if (signalled)
                    {
                        passes = std::min(g_frames, kMaxPassesPerWake);
                        g_frames = 0;
                        lastSignal = now;
                    }
                    else if (now - lastSignal > kDriverTimeout)
                    {
                        passes = 1;
                    }
                }
                std::lock_guard lock(g_mutex);
                for (uint32_t pass = 0; pass < passes; pass++)
                {
                    for (uint32_t i = 0; i < kContextCount; i++)
                    {
                        Context& c = g_contexts[i];
                        if (c.allocated && c.enabled)
                            Service(c, Address(i));
                    }
                }
            }
        }
    }

    void InitializeXma()
    {
        g_array = mem::Alloc(mem::Heap::Physical, kContextCount * kXmaContextSize, 0x1000);
        if (g_array == 0)
            Fatal("xma: cannot allocate the contexts");
        // The register is read byte-reversed (lwbrx), so it holds the base little-endian.
        mem::Commit(kContextArrayRegister, 4);
        mem::Store32(kContextArrayRegister, __builtin_bswap32(g_array & kPhysicalMask));
        std::thread(ApuThread).detach();
    }

    uint32_t AllocateContext()
    {
        std::lock_guard lock(g_mutex);
        for (uint32_t i = 0; i < kContextCount; i++)
        {
            Context& c = g_contexts[i];
            if (c.allocated)
                continue;
            c.allocated = true;
            c.enabled = false;
            ResetStream(c);
            memset(mem::Ptr(Address(i)), 0, kXmaContextSize);
            return Address(i);
        }
        return 0;
    }

    void ReleaseContext(uint32_t context)
    {
        std::lock_guard lock(g_mutex);
        Context* c = Find(context);
        if (c == nullptr)
            return;
        c->allocated = false;
        c->enabled = false;
        c->decoder.Close();
        c->pending = {};
        c->pendingRead = 0;
    }

    uint32_t ContextIndex(uint32_t context)
    {
        std::lock_guard lock(g_mutex);
        uint32_t index;
        return Find(context, &index) != nullptr ? index : kNoXmaContext;
    }

    void InitializeContext(uint32_t context, const XmaContextInit& init)
    {
        std::lock_guard lock(g_mutex);
        Context* c = Find(context);
        if (c == nullptr)
            return;
        c->enabled = false;
        ResetStream(*c);
        memset(mem::Ptr(context), 0, kXmaContextSize);
        StoreField(context, kInput0PacketCount, init.inputBuffer0PacketCount);
        StoreField(context, kLoopCount, init.loopCount);
        StoreField(context, kOutputBlockCount, init.outputBufferBlockCount);
        StoreField(context, kInput1PacketCount, init.inputBuffer1PacketCount);
        StoreField(context, kLoopSubframeEnd, init.loopSubframeEnd);
        StoreField(context, kLoopSubframeSkip, init.loopSubframeSkip);
        StoreField(context, kSubframeDecodeCount, init.subframeDecodeCount);
        StoreField(context, kSampleRate, init.sampleRate);
        StoreField(context, kStereo, init.stereo);
        StoreField(context, kInputReadOffset, init.inputBufferReadOffset);
        StoreField(context, kLoopStart, init.loopStart);
        StoreField(context, kLoopEnd, init.loopEnd);
        mem::Store32(context + kInput0Pointer * 4, init.inputBuffer0 & kPhysicalMask);
        mem::Store32(context + kInput1Pointer * 4, init.inputBuffer1 & kPhysicalMask);
        mem::Store32(context + kOutputPointer * 4, init.outputBuffer & kPhysicalMask);
        mem::Store32(context + kWorkPointer * 4, init.workBuffer & kPhysicalMask);
    }

    void SignalAudioFrame()
    {
        {
            std::lock_guard lock(g_frameMutex);
            g_frames++;
        }
        g_frameSignal.notify_one();
    }

    void EnableContext(uint32_t index)
    {
        std::lock_guard lock(g_mutex);
        if (index < kContextCount && g_contexts[index].allocated)
            g_contexts[index].enabled = true;
    }

    void DisableContext(uint32_t index)
    {
        std::lock_guard lock(g_mutex);
        if (index < kContextCount)
            g_contexts[index].enabled = false;
    }

    void ClearContext(uint32_t index)
    {
        std::lock_guard lock(g_mutex);
        if (index < kContextCount && g_contexts[index].allocated)
            ResetStream(g_contexts[index]);
    }

    bool IsInputBufferValid(uint32_t context, int buffer)
    {
        std::lock_guard lock(g_mutex);
        return LoadField(context, buffer == 0 ? kInput0Valid : kInput1Valid) != 0;
    }

    void SetInputBufferValid(uint32_t context, int buffer)
    {
        std::lock_guard lock(g_mutex);
        StoreField(context, buffer == 0 ? kInput0Valid : kInput1Valid, 1);
    }

    bool IsOutputBufferValid(uint32_t context)
    {
        std::lock_guard lock(g_mutex);
        return LoadField(context, kOutputValid) != 0;
    }

    void SetOutputBufferValid(uint32_t context)
    {
        std::lock_guard lock(g_mutex);
        StoreField(context, kOutputValid, 1);
    }

    uint32_t GetOutputBufferReadOffset(uint32_t context)
    {
        std::lock_guard lock(g_mutex);
        return LoadField(context, kOutputReadOffset);
    }

    void SetOutputBufferReadOffset(uint32_t context, uint32_t offset)
    {
        std::lock_guard lock(g_mutex);
        StoreField(context, kOutputReadOffset, offset);
    }

    uint32_t GetOutputBufferWriteOffset(uint32_t context)
    {
        std::lock_guard lock(g_mutex);
        return LoadField(context, kOutputWriteOffset);
    }
}
