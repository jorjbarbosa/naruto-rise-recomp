// One XMA frame through FFmpeg's xmaframes decoder (decoder.h).
#include "audio/xma/decoder.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
}

namespace narutorise::audio::xma {
namespace {
const AVCodec* Codec() {
  static const AVCodec* codec = [] {
    av_log_set_level(AV_LOG_QUIET);  // Failures are counted and reported by the contexts.
    return avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
  }();
  return codec;
}
std::string Error(const char* call, int code) {
  char text[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(code, text, sizeof(text));
  return std::string(call) + ": " + text;
}
}  // namespace

Decoder::~Decoder() {
  if (context_) avcodec_free_context(&context_);
  if (frame_) av_frame_free(&frame_);
  if (packet_) av_packet_free(&packet_);
}

void Decoder::Reset() {
  // Reopening is what clears the transform's history (xmaframes has no flush).
  if (context_) avcodec_free_context(&context_);
  rate_ = channels_ = 0;
}

bool Decoder::Open(int rate, int channels) {
  if (context_ && rate == rate_ && channels == channels_) return true;
  if (context_) avcodec_free_context(&context_);
  rate_ = channels_ = 0;
  if (!Codec()) {
    last_error_ = "no xmaframes decoder";
    return false;
  }
  if (!frame_) frame_ = av_frame_alloc();
  if (!packet_) packet_ = av_packet_alloc();
  context_ = avcodec_alloc_context3(Codec());
  if (!context_ || !frame_ || !packet_) {
    last_error_ = "out of memory";
    return false;
  }
  context_->sample_rate = rate;
  context_->channels = channels;
  if (const int code = avcodec_open2(context_, Codec(), nullptr); code < 0) {
    last_error_ = Error("avcodec_open2", code);
    avcodec_free_context(&context_);
    return false;
  }
  rate_ = rate;
  channels_ = channels;
  ++opens_;
  return true;
}

bool Decoder::Decode(uint8_t* packet, uint32_t size, int rate, int channels, uint8_t* out) {
  if (!Open(rate, channels)) return false;
  packet_->data = packet;
  packet_->size = static_cast<int>(size);
  if (const int code = avcodec_send_packet(context_, packet_); code < 0) {
    last_error_ = Error("avcodec_send_packet", code);
    return false;
  }
  if (const int code = avcodec_receive_frame(context_, frame_); code < 0) {
    last_error_ = Error("avcodec_receive_frame", code);
    return false;
  }
  if (frame_->nb_samples != static_cast<int>(kSamplesPerFrame)) {
    last_error_ = "unexpected frame of " + std::to_string(frame_->nb_samples) + " samples";
    return false;
  }
  const float* planes[2] = {reinterpret_cast<const float*>(frame_->data[0]),
                            reinterpret_cast<const float*>(channels > 1 ? frame_->data[1] : frame_->data[0])};
  ToPcm16(planes, channels, kSamplesPerFrame, out);
  return true;
}

void ToPcm16(const float* const* planes, int channels, uint32_t samples, uint8_t* out) {
  for (uint32_t i = 0; i < samples; ++i) {
    for (int c = 0; c < channels; ++c) {
      float v = planes[c][i];
      if (std::isnan(v)) v = 0.0f;
      const auto pcm = static_cast<int16_t>(std::lrint(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
      const uint16_t be = std::byteswap(static_cast<uint16_t>(pcm));
      std::memcpy(out + (size_t(i) * channels + c) * 2, &be, 2);
    }
  }
}
}  // namespace narutorise::audio::xma
