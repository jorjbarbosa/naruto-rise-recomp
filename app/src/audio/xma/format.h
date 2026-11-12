#pragma once

// The 64-byte XMA context the title and the hardware share in guest physical
// memory (stage 2c of the native audio): 16 big-endian words, read as bit
// fields after a swap.
//
// The layout is the hardware's; the title fills it in sub_82151D80 (XAudio
// voices) and through XMAInitializeContext (DARE streams, sub_822BF278), and
// reads the progress fields back (sub_82152328, sub_822B96E8). The fields this
// title was seen to use (nota-de-desenho-2a.md) are commented; the others are
// kept only for the layout and counted when set.
#include <bit>
#include <cstdint>
#include <cstring>

namespace narutorise::audio::xma {
constexpr uint32_t kContextBytes = 64;
// The output ring's unit: one subframe of 128 16-bit samples of one channel.
constexpr uint32_t kOutputBlockBytes = 256;
constexpr int kSampleRates[4] = {24000, 32000, 44100, 48000};

struct ContextData {
  // DWORD 0
  uint32_t input_buffer_0_packet_count : 12;  // Packets in buffer 0.
  uint32_t loop_count : 8;                    // 0 none, 255 endless (the only values seen).
  uint32_t input_buffer_0_valid : 1;
  uint32_t input_buffer_1_valid : 1;
  uint32_t output_buffer_block_count : 5;  // Ring size in blocks (15, 30 or 31 seen).
  uint32_t output_buffer_write_offset : 5;  // In blocks; the hardware's.
  // DWORD 1
  uint32_t input_buffer_1_packet_count : 12;
  uint32_t loop_subframe_end : 2;   // Last subframe played of the loop's last frame.
  uint32_t unknown_1a : 3;
  uint32_t loop_subframe_skip : 3;  // Subframes skipped of the loop's first frame.
  uint32_t subframe_decode_count : 4;  // 2 (DARE) or 8 (XAudio) seen.
  uint32_t output_buffer_padding : 3;  // Always 0 seen.
  uint32_t sample_rate : 2;            // Index into kSampleRates.
  uint32_t is_stereo : 1;
  uint32_t unknown_1c : 1;
  uint32_t output_buffer_valid : 1;
  // DWORD 2
  uint32_t input_buffer_read_offset : 26;  // Bit offset in the current buffer.
  uint32_t error_status : 5;
  uint32_t error_set : 1;
  // DWORD 3
  uint32_t loop_start : 26;  // Bit offset in buffer 0.
  uint32_t parser_error_status : 5;
  uint32_t parser_error_set : 1;
  // DWORD 4
  uint32_t loop_end : 26;  // Bit offset in buffer 0.
  uint32_t packet_metadata : 5;
  uint32_t current_buffer : 1;
  // DWORD 5-8: physical addresses.
  uint32_t input_buffer_0_ptr;
  uint32_t input_buffer_1_ptr;
  uint32_t output_buffer_ptr;
  uint32_t work_buffer_ptr;
  // DWORD 9
  uint32_t output_buffer_read_offset : 5;  // In blocks; the consumer's.
  uint32_t : 25;
  uint32_t stop_when_done : 1;       // Never set by this title.
  uint32_t interrupt_when_done : 1;  // Never set by this title.
  // DWORD 10-15
  uint32_t unknown_10_15[6];

  static ContextData Load(const uint8_t* guest) {
    uint32_t words[16];
    std::memcpy(words, guest, sizeof(words));
    for (uint32_t& word : words) word = std::byteswap(word);
    ContextData data;
    std::memcpy(&data, words, sizeof(words));
    return data;
  }
  void Store(uint8_t* guest) const {
    uint32_t words[16];
    std::memcpy(words, this, sizeof(words));
    for (uint32_t& word : words) word = std::byteswap(word);
    std::memcpy(guest, words, sizeof(words));
  }
};
static_assert(sizeof(ContextData) == kContextBytes);
}  // namespace narutorise::audio::xma
