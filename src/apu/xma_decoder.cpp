// One XMA stream decoded with FFmpeg's xma1 decoder.

#include "apu/xma_decoder.h"

#include "log.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace apu
{
    namespace
    {
        // XMAWAVEFORMAT as FFmpeg takes it (from EncodeOptions on): 8 bytes, then 20 per stream.
        constexpr int kExtradataSize = 8 + 20;
        constexpr int kExtradataStreams = 4;
        constexpr int kExtradataChannels = 8 + 17;
        // FFmpeg's own messages, up to this many, so a broken stream cannot flood the log.
        constexpr int kMaxFfmpegMessages = 32;

        int g_ffmpegMessages = 0;

        // Called under the XMA hardware lock, like every FFmpeg call.
        void FfmpegLog(void*, int level, const char* format, va_list args)
        {
            if (level > AV_LOG_ERROR || g_ffmpegMessages >= kMaxFfmpegMessages)
                return;
            char message[512];
            vsnprintf(message, sizeof(message), format, args);
            size_t length = strlen(message);
            while (length > 0 && message[length - 1] == '\n')
                message[--length] = 0;
            Log("xma: ffmpeg: %s%s", message, ++g_ffmpegMessages == kMaxFfmpegMessages ? " (no more ffmpeg messages)" : "");
        }

        int16_t ToPcm16(float sample)
        {
            return int16_t(std::lrintf(std::clamp(sample, -1.0f, 1.0f) * 32767.0f));
        }
    }

    XmaDecoder::~XmaDecoder()
    {
        Close();
    }

    bool XmaDecoder::Open(uint32_t sampleRate, uint32_t channels)
    {
        static bool logInstalled = false;
        if (!logInstalled)
        {
            av_log_set_callback(FfmpegLog);
            logInstalled = true;
        }

        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA1);
        codec_ = codec != nullptr ? avcodec_alloc_context3(codec) : nullptr;
        packet_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        if (codec_ == nullptr || packet_ == nullptr || frame_ == nullptr)
        {
            Log("error: xma: cannot allocate the FFmpeg decoder");
            Close();
            return false;
        }
        codec_->sample_rate = int(sampleRate);
        av_channel_layout_default(&codec_->ch_layout, int(channels));
        codec_->extradata = static_cast<uint8_t*>(av_mallocz(kExtradataSize + AV_INPUT_BUFFER_PADDING_SIZE));
        if (codec_->extradata == nullptr)
        {
            Close();
            return false;
        }
        codec_->extradata_size = kExtradataSize;
        codec_->extradata[kExtradataStreams] = 1;
        codec_->extradata[kExtradataChannels] = uint8_t(channels);
        int result = avcodec_open2(codec_, codec, nullptr);
        if (result < 0)
        {
            Log("error: xma: cannot open the decoder for %u Hz, %u channels (%d)", sampleRate, channels, result);
            Close();
            return false;
        }
        sampleRate_ = sampleRate;
        channels_ = channels;
        return true;
    }

    void XmaDecoder::DecodePacket(const uint8_t* packet, uint32_t sampleRate, uint32_t channels, std::vector<int16_t>& out)
    {
        if (codec_ != nullptr && (sampleRate != sampleRate_ || channels != channels_))
            Close();
        if (codec_ == nullptr)
        {
            // A format that failed to open is not retried on every packet.
            if (openFailed_ && sampleRate == sampleRate_ && channels == channels_)
                return;
            openFailed_ = !Open(sampleRate, channels);
            sampleRate_ = sampleRate;
            channels_ = channels;
            if (openFailed_)
                return;
        }

        // A fresh padded buffer each time: the decoder may keep a reference to the last one.
        av_packet_unref(packet_);
        if (av_new_packet(packet_, kXmaPacketSize) < 0)
            return;
        memcpy(packet_->data, packet, kXmaPacketSize);
        if (avcodec_send_packet(codec_, packet_) < 0)
            return;
        while (avcodec_receive_frame(codec_, frame_) == 0)
        {
            int frameChannels = std::min(frame_->ch_layout.nb_channels, int(channels));
            size_t start = out.size();
            out.resize(start + size_t(frame_->nb_samples) * channels);
            for (int c = 0; c < frameChannels; c++)
            {
                const float* samples = reinterpret_cast<const float*>(frame_->extended_data[c]);
                for (int i = 0; i < frame_->nb_samples; i++)
                    out[start + size_t(i) * channels + c] = ToPcm16(samples[i]);
            }
            av_frame_unref(frame_);
        }
    }

    void XmaDecoder::Reset()
    {
        if (codec_ != nullptr)
            avcodec_flush_buffers(codec_);
    }

    void XmaDecoder::Close()
    {
        avcodec_free_context(&codec_);
        av_packet_free(&packet_);
        av_frame_free(&frame_);
        sampleRate_ = 0;
        channels_ = 0;
        openFailed_ = false;
    }
}
