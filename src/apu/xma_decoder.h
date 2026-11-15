#pragma once

#include <cstdint>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace apu
{
    constexpr uint32_t kXmaPacketSize = 2048;

    // One XMA stream (1 or 2 channels) decoded packet by packet with FFmpeg's xma1 decoder.
    //
    // FFmpeg is built without threads: every XmaDecoder of the process must be used under one lock
    // (the XMA hardware lock in xma.cpp).
    class XmaDecoder
    {
    public:
        XmaDecoder() = default;
        XmaDecoder(const XmaDecoder&) = delete;
        XmaDecoder& operator=(const XmaDecoder&) = delete;
        ~XmaDecoder();

        // Decodes one packet (kXmaPacketSize bytes) of a stream with this format and appends its
        // samples to out, interleaved when stereo. Packets continue the stream of the previous
        // call, so frames that span packets decode whole; a new format starts a new stream. A
        // decoder that cannot be opened, or a packet that does not decode, adds nothing (logged).
        void DecodePacket(const uint8_t* packet, uint32_t sampleRate, uint32_t channels, std::vector<int16_t>& out);
        // Forgets the stream: the next packet starts a new one.
        void Reset();
        // Frees FFmpeg's state; the next packet opens the decoder again.
        void Close();

    private:
        bool Open(uint32_t sampleRate, uint32_t channels);

        AVCodecContext* codec_ = nullptr;
        AVPacket* packet_ = nullptr;
        AVFrame* frame_ = nullptr;
        uint32_t sampleRate_ = 0;
        uint32_t channels_ = 0;
        bool openFailed_ = false;
    };
}
