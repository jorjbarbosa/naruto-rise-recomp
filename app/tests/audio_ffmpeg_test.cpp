// The native audio's FFmpeg (app/cmake/ffmpeg.cmake): only the XMA frames
// decoder is registered, it opens as the XMA contexts open it (mono and stereo,
// the four XMA rates, planar float), and a frame it cannot use is refused
// without a crash.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
}

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int checks = 0;
void Check(bool value, const std::string& message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

void Registered() {
  std::vector<std::string> names;
  void* state = nullptr;
  while (const AVCodec* codec = av_codec_iterate(&state)) names.push_back(codec->name);
  Check(names.size() == 1 && names[0] == "xmaframes",
        "Only xmaframes is registered (" + std::to_string(names.size()) + " codecs)");
  Check(avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES) != nullptr, "The XMA frames decoder is found by its id");
  Check(avcodec_find_decoder(AV_CODEC_ID_MP3) == nullptr, "No other decoder (MP3)");
  Check(avcodec_find_decoder(AV_CODEC_ID_WMAV2) == nullptr, "No other decoder (WMA v2)");
}

void Opens() {
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
  for (int channels : {1, 2}) {
    for (int rate : {24000, 32000, 44100, 48000}) {
      AVCodecContext* context = avcodec_alloc_context3(codec);
      Check(context != nullptr, "A context is allocated");
      context->channels = channels;
      context->sample_rate = rate;
      Check(avcodec_open2(context, codec, nullptr) == 0,
            "Opens with " + std::to_string(channels) + " channels at " + std::to_string(rate));
      Check(context->sample_fmt == AV_SAMPLE_FMT_FLTP, "Planar float output");
      // A frame of zeros: the decoder refuses it or gives a frame, never a crash.
      std::vector<uint8_t> data(64 + AV_INPUT_BUFFER_PADDING_SIZE, 0);
      AVPacket* packet = av_packet_alloc();
      AVFrame* frame = av_frame_alloc();
      packet->data = data.data();
      packet->size = 64;
      const int sent = avcodec_send_packet(context, packet);
      if (sent == 0) (void)avcodec_receive_frame(context, frame);
      avcodec_flush_buffers(context);
      av_frame_free(&frame);
      av_packet_free(&packet);
      avcodec_free_context(&context);
      Check(context == nullptr, "The context is freed");
    }
  }
}
}  // namespace

int main() {
  av_log_set_level(AV_LOG_QUIET);  // The refused frames are expected.
  try {
    Registered();
    Opens();
  } catch (const std::exception& error) {
    std::cerr << "FAIL after " << checks << " checks: " << error.what() << "\n";
    return 1;
  }
  std::cout << "PASS: " << checks << " checks (" << avcodec_configuration() << ")\n";
  return 0;
}
