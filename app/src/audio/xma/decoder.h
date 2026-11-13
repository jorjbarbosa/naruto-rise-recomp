#pragma once

// One XMA frame to 512 samples per channel (stage 2c of the native audio),
// through the app's FFmpeg (app/cmake/ffmpeg.cmake, the xmaframes decoder).
//
// The context hands it the frame as stream.h cuts it (byte 0 the padding
// lengths, the frame from byte 1). The output is what the hardware writes to a
// context's output buffer: 16-bit big-endian samples, channels interleaved.
// The decoder keeps the stream's history between frames (overlap of its
// transform); Reset starts a new stream (a new or cleared context).
#include <cstdint>
#include <string>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace narutorise::audio::xma {
constexpr uint32_t kSamplesPerFrame = 512;

// What a context decodes with: the app's FFmpeg (Decoder) or, in tests, a
// decoder whose samples say where they come from.
class FrameDecoder {
 public:
  virtual ~FrameDecoder() = default;
  // Decodes `size` bytes at `packet` for a stream of `rate` Hz and `channels`
  // (1 or 2); on success writes kSamplesPerFrame x channels samples to `out`.
  virtual bool Decode(uint8_t* packet, uint32_t size, int rate, int channels, uint8_t* out) = 0;
  // A new stream: the history between frames is dropped.
  virtual void Reset() = 0;
};

class Decoder final : public FrameDecoder {
 public:
  Decoder() = default;
  ~Decoder() override;
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;

  bool Decode(uint8_t* packet, uint32_t size, int rate, int channels, uint8_t* out) override;
  void Reset() override;
  const std::string& LastError() const { return last_error_; }
  uint64_t Opens() const { return opens_; }

 private:
  bool Open(int rate, int channels);
  AVCodecContext* context_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* packet_ = nullptr;
  int rate_ = 0;
  int channels_ = 0;
  uint64_t opens_ = 0;
  std::string last_error_;
};

// Planar float to interleaved 16-bit big-endian: clamped to [-1, 1], scaled
// by 32767 and rounded to the nearest integer.
void ToPcm16(const float* const* planes, int channels, uint32_t samples, uint8_t* out);
}  // namespace narutorise::audio::xma
