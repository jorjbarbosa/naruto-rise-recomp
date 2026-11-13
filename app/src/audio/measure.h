#pragma once

// Stage 2a of the native audio: what the title asks of the XMA hardware, measured
// in play (NARUTORISE_AUDIO_MEASURE=1). Off by default; nothing changes in what
// the title gets, only counts are taken.
//
// - Voices created by the title's XAudio (sub_82149198, XAudioCreateSourceVoice):
//   by the init's first byte (1 = 16-bit PCM, 2 = 8-bit PCM, 4 = XMA) and by the
//   caller's region (DARE, Bink, the 8250D library, XAudio itself, other).
//   Type 4 is the XMA path A: contexts driven through the registers.
// - DARE stream decoders (sub_822B84F0): by format, r5 (1 PCM, 2 Ubisoft ADPCM,
//   3 IMA-ADPCM, 4 none, 5 XMA through the XMA* imports: path B).
// - The XMA registers at 0x7FEA0000: reads and writes per register, and the
//   values written to 0x601 (the timeout path). The SDK's MMIO callbacks are
//   wrapped when the title's XAudio starts (sub_82148EB8 reads 0x600 first).
// - The XMA* imports: calls per import, through a counting gate over the SDK's
//   exports (kernel_xma.cpp).
// - The profile of each context the title starts: at a Kick through the
//   registers (path A) and at XMAEnableContext (path B), the fields it set
//   (packets per input buffer, output blocks, subframes per decode, padding,
//   rate, channels, loop, flags), counted by distinct profile. They bound what
//   the native contexts must do for this title.
// The counts go to the log every 10 s ("[narutorise] Audio measure:") and at the
// close, as one JSON object of running totals.
#include <cstdint>
#include <string>

namespace narutorise::audio::measure {
// At the start (audio::Configure).
void Configure();
bool Enabled();
// One import of the gate (kernel_xma.cpp): its slot, then a call.
int RegisterImport(const char* name);
void CountImport(int slot);
// A context the title starts, its 64 bytes as in guest memory (big-endian).
void NoteContext(char path, const uint8_t* context);
// After an XMA* import returned: `context` its first argument, `result` its r3.
void AfterImport(const char* name, uint32_t context, uint32_t result);
// Path B: the XMA_CONTEXT_INIT the title passes to XMAInitializeContext (guest address).
void NoteInit(uint32_t guest_init);
std::string Json();
// At the close: the DARE streams still open, as if they ended.
void FlushStreams();
}  // namespace narutorise::audio::measure
