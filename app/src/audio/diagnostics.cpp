// The diagnostics of the native audio layer (diagnostics.h).
#include "audio/diagnostics.h"
#include "audio/mix.h"
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <rex/logging.h>

namespace audio::diagnostics {
namespace {
Counters counters;

// The recording: a WAV file whose sizes are rewritten every second, so a
// session that ends with a hard exit still leaves a readable file.
struct Recording {
  std::mutex mutex;
  std::ofstream file;
  bool tried = false;
  uint32_t frames = 0, written = 0;
};
Recording recording;

void PutU32(std::array<uint8_t, 44>& header, size_t at, uint32_t value) {
  for (int i = 0; i < 4; ++i) header[at + i] = uint8_t(value >> (8 * i));
}
void WriteHeader(std::ofstream& file, uint32_t frames) {
  const uint32_t data = frames * kFrameBytes;
  std::array<uint8_t, 44> header{};
  std::memcpy(header.data(), "RIFF", 4);
  PutU32(header, 4, 36 + data);
  std::memcpy(header.data() + 8, "WAVEfmt ", 8);
  PutU32(header, 16, 16);
  header[20] = 3;  // IEEE float
  header[22] = uint8_t(kFrameChannels);
  PutU32(header, 24, kSampleRate);
  PutU32(header, 28, kSampleRate * kFrameChannels * 4);
  header[32] = uint8_t(kFrameChannels * 4);
  header[34] = 32;
  std::memcpy(header.data() + 36, "data", 4);
  PutU32(header, 40, data);
  file.seekp(0);
  file.write(reinterpret_cast<const char*>(header.data()), header.size());
  file.seekp(0, std::ios::end);
}
}  // namespace

Counters& Get() { return counters; }

void Report(const char* reason) {
  REXLOG_INFO("[narutorise] Audio ({}): {} periods, {} callbacks, {} frames native, {} through the SDK; "
              "largest block {} samples, queue limit {}; {} underruns, {} periods without a callback, "
              "{} with two, {} grid restarts, {} device stalls",
              reason, counters.periods.load(), counters.callbacks.load(), counters.frames.load(),
              counters.sdk_frames.load(), counters.largest_block.load(), counters.queue_limit.load(),
              counters.underruns.load(), counters.skipped.load(), counters.doubled.load(),
              counters.restarts.load(), counters.stalls.load());
}

void Record(const uint8_t* frame) {
  std::lock_guard lock(recording.mutex);
  if (!recording.tried) {
    recording.tried = true;
    const char* path = std::getenv("REX_NATIVE_AUDIO_DUMP");
    if (!path || !*path) return;
    recording.file.open(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
    if (!recording.file) {
      REXLOG_WARN("[narutorise] Audio: the recording {} could not be created", path);
      return;
    }
    WriteHeader(recording.file, 0);
    REXLOG_INFO("[narutorise] Audio: recording the title's frames into {}", path);
  }
  if (!recording.file.is_open()) return;
  std::array<float, kFrameSamples * kFrameChannels> samples;
  Interleave(frame, samples.data());
  recording.file.write(reinterpret_cast<const char*>(samples.data()), sizeof(samples));
  if (++recording.frames - recording.written >= kSampleRate / kFrameSamples) {
    WriteHeader(recording.file, recording.frames);
    recording.written = recording.frames;
  }
}

void CloseRecording() {
  std::lock_guard lock(recording.mutex);
  if (!recording.file.is_open()) return;
  WriteHeader(recording.file, recording.frames);
  recording.file.close();
  REXLOG_INFO("[narutorise] Audio: recording closed, {} frames", recording.frames);
}
}  // namespace audio::diagnostics
