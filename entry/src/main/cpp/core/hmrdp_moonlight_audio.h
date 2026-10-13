#ifndef HMRDP_MOONLIGHT_AUDIO_H
#define HMRDP_MOONLIGHT_AUDIO_H

#include <cstddef>
#include <cstdint>

namespace hmrdp {

// AUDIO_RENDERER_CALLBACKS table for LiStartConnection: Opus multistream into the shared
// AudioSink ring; null disables audio. Same shape as MoonlightVideoCallbacks().
void* MoonlightAudioCallbacks();

// true: a native OHAudio renderer (FAST + audio workgroup) pulls the ring; false: the ArkTS
// worker does. Set before the session starts.
void MoonlightAudioSetLowLatency(bool enabled);

// Pull entry for the ArkTS audio worker; silence-fills an underrun.
size_t MoonlightAudioRead(uint8_t* dest, size_t size);

// The decoder's PCM format (0/0 before init); known from init, not the first packet, so a
// renderer can seed itself while the host is still silent.
void MoonlightAudioFormat(int* sampleRate, int* channels);

// Drops queued PCM and forgets the format/decoder; call around session start/stop so a new
// session never plays stale samples.
void MoonlightAudioReset();

}

#endif
