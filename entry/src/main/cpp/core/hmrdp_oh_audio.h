#ifndef HMRDP_OH_AUDIO_H
#define HMRDP_OH_AUDIO_H

namespace hmrdp {

class AudioSink;

// Moonlight low-latency output: a native OH_AudioRenderer (FAST mode) that pulls
// `ring` in its write callback, sized to the device callback period. False on
// failure; the caller then stays silent.
bool OhAudioStart(AudioSink* ring, int sampleRate, int channels);

// Stops and releases the renderer and its audio workgroup. Idempotent.
void OhAudioStop();

// Producer-side workgroup: call around one decode + enqueue, on the decode
// thread. No-ops when no workgroup is active.
void OhAudioWorkgroupBegin();
void OhAudioWorkgroupEnd();

}  // namespace hmrdp

#endif  // HMRDP_OH_AUDIO_H
