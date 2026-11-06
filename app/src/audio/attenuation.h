#pragma once

// The music attenuation (music_attenuation): while another app plays media,
// the game's music fades out (render.h). On Windows the media sessions come
// from the system's media transport controls (Windows 10 1809 and later);
// elsewhere no other media is seen.
namespace audio::attenuation {
#ifdef _WIN32
// Starts or stops watching the media sessions, twice a second.
void Enable(bool on);
// True while another app plays media; false when not watching.
bool OtherMediaPlaying();
#else
inline void Enable(bool) {}
inline bool OtherMediaPlaying() { return false; }
#endif
}  // namespace audio::attenuation
