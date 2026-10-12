#ifndef HMRDP_MOONLIGHT_AUDIO_H
#define HMRDP_MOONLIGHT_AUDIO_H

#include <cstddef>
#include <cstdint>

namespace hmrdp {

// AUDIO_RENDERER_CALLBACKS table for LiStartConnection: Opus multistream decode
// into the shared AudioSink ring. Null disables the audio stream. Same shape as
// MoonlightVideoCallbacks().
void* MoonlightAudioCallbacks();

// true: a native OHAudio renderer (FAST + audio workgroup) pulls the ring
// itself; false: the ArkTS audio worker does. Set before the session starts.
void MoonlightAudioSetLowLatency(bool enabled);

// Pull entry for the ArkTS audio worker: copies the current-format PCM into dest,
// silence-fills an underrun, and returns the real byte count.
size_t MoonlightAudioRead(uint8_t* dest, size_t size);

// The decoder's configured PCM format (0/0 before init). Known from init, not
// from the first packet, so a renderer started right after the session connects
// can seed itself even while the host is still silent.
void MoonlightAudioFormat(int* sampleRate, int* channels);

// Drops queued PCM and forgets the format/decoder; call around session start and
// stop so a new session never plays stale samples.
void MoonlightAudioReset();

}  // namespace hmrdp

#endif  // HMRDP_MOONLIGHT_AUDIO_H
