#include "hmrdp_oh_audio.h"

#include <atomic>
#include <chrono>
#include <mutex>

#include <ohaudio/native_audio_resource_manager.h>
#include <ohaudio/native_audiorenderer.h>
#include <ohaudio/native_audiostreambuilder.h>

#include "hmrdp_audio.h"
#include "hmrdp_log.h"

namespace hmrdp {
namespace {

// Ring budget in device callback periods: prefill two so a late packet does not
// underrun, cap at four so latency cannot creep.
constexpr int kTargetCallbackPeriods = 2;
constexpr int kHighWaterCallbackPeriods = 4;

// Generous upper bound for one decode + enqueue (matches the official example).
constexpr uint64_t kWorkgroupDeadlineMs = 20;

std::mutex g_mutex;
std::atomic<AudioSink*> g_ring{nullptr};
OH_AudioRenderer* g_renderer = nullptr;
OH_AudioResourceManager* g_resourceManager = nullptr;
OH_AudioWorkgroup* g_workgroup = nullptr;
int32_t g_workgroupToken = 0;
// Producer thread registration: 0 not tried, 1 registered, 2 failed (no retry,
// the per-decode path would log-spam).
int g_threadState = 0;

// Runs on the OHAudio thread; only reads the ring pointer, so Stop() never waits
// on this callback for g_mutex.
OH_AudioData_Callback_Result OnWriteData(OH_AudioRenderer* /*renderer*/, void* /*userData*/,
                                         void* audioData, int32_t audioDataSize) {
  AudioSink* ring = g_ring.load();
  if (ring != nullptr && audioData != nullptr && audioDataSize > 0) {
    ring->Read(static_cast<uint8_t*>(audioData), static_cast<size_t>(audioDataSize));
  }
  // The ring always fills the whole buffer (audio + underrun silence).
  return AUDIO_DATA_CALLBACK_RESULT_VALID;
}

uint64_t NowMs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}

void ReleaseWorkgroupLocked() {
  if (g_workgroup != nullptr && g_resourceManager != nullptr) {
    // RemoveThread() must run on the added thread; Stop() may not be, so release.
    OH_AudioResourceManager_ReleaseWorkgroup(g_resourceManager, g_workgroup);
  }
  g_workgroup = nullptr;
  g_workgroupToken = 0;
  g_threadState = 0;
}

}  // namespace

bool OhAudioStart(AudioSink* ring, int sampleRate, int channels) {
  if (ring == nullptr || sampleRate <= 0 || channels <= 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_renderer != nullptr) {
    return true;
  }

  OH_AudioStreamBuilder* builder = nullptr;
  if (OH_AudioStreamBuilder_Create(&builder, AUDIOSTREAM_TYPE_RENDERER) != AUDIOSTREAM_SUCCESS
      || builder == nullptr) {
    HMRDP_LOGE("ohaudio: builder create failed");
    return false;
  }
  OH_AudioStreamBuilder_SetSamplingRate(builder, sampleRate);
  OH_AudioStreamBuilder_SetChannelCount(builder, channels);
  OH_AudioStreamBuilder_SetSampleFormat(builder, AUDIOSTREAM_SAMPLE_S16LE);
  OH_AudioStreamBuilder_SetEncodingType(builder, AUDIOSTREAM_ENCODING_TYPE_RAW);
  // GAME picks the low-latency-oriented route; FAST asks for the small buffer.
  // Both are advisory (surfaced by GetFastStatus below).
  OH_AudioStreamBuilder_SetRendererInfo(builder, AUDIOSTREAM_USAGE_GAME);
  OH_AudioStreamBuilder_SetLatencyMode(builder, AUDIOSTREAM_LATENCY_MODE_FAST);
  OH_AudioStreamBuilder_SetRendererWriteDataCallback(builder, OnWriteData, nullptr);

  g_ring.store(ring);
  OH_AudioRenderer* renderer = nullptr;
  const OH_AudioStream_Result generated =
      OH_AudioStreamBuilder_GenerateRenderer(builder, &renderer);
  OH_AudioStreamBuilder_Destroy(builder);
  if (generated != AUDIOSTREAM_SUCCESS || renderer == nullptr) {
    HMRDP_LOGE("ohaudio: generate renderer failed (%{public}d)", static_cast<int>(generated));
    g_ring.store(nullptr);
    return false;
  }

  // Size the ring to the device period before the first callback can underrun.
  int32_t frames = 0;
  if (OH_AudioRenderer_GetFrameSizeInCallback(renderer, &frames) == AUDIOSTREAM_SUCCESS
      && frames > 0) {
    const int periodMs = static_cast<int>(frames) * 1000 / sampleRate;
    const int targetMs = periodMs > 0 ? periodMs * kTargetCallbackPeriods
                                      : kTargetCallbackPeriods;
    const int highWaterMs = periodMs * kHighWaterCallbackPeriods;
    ring->SetLatencyBudget(targetMs, highWaterMs);
    HMRDP_LOGI("ohaudio: callback %{public}d frames (%{public}d ms), ring %{public}d/%{public}d ms",
               frames, periodMs, targetMs, highWaterMs);
  }

  if (OH_AudioRenderer_Start(renderer) != AUDIOSTREAM_SUCCESS) {
    HMRDP_LOGE("ohaudio: renderer start failed");
    OH_AudioRenderer_Release(renderer);
    g_ring.store(nullptr);
    return false;
  }
  g_renderer = renderer;

  OH_AudioStream_FastStatus fast = AUDIOSTREAM_FASTSTATUS_NORMAL;
  if (OH_AudioRenderer_GetFastStatus(renderer, &fast) == AUDIOSTREAM_SUCCESS) {
    HMRDP_LOGI("ohaudio: playback started (fast=%{public}d)",
               fast == AUDIOSTREAM_FASTSTATUS_FAST ? 1 : 0);
  } else {
    HMRDP_LOGI("ohaudio: playback started");
  }

  // Best-effort: failure only loses the scheduling guarantee, so it is logged
  // rather than fatal. The decode thread registers on its first decode.
  if (OH_AudioManager_GetAudioResourceManager(&g_resourceManager) != AUDIOCOMMON_RESULT_SUCCESS
      || g_resourceManager == nullptr) {
    HMRDP_LOGW("ohaudio: workgroup unavailable (no resource manager)");
  } else if (OH_AudioResourceManager_CreateWorkgroup(g_resourceManager, "hmrdp-moonlight",
                                                     &g_workgroup) != AUDIOCOMMON_RESULT_SUCCESS) {
    g_workgroup = nullptr;
    HMRDP_LOGW("ohaudio: workgroup create failed; playing without it");
  } else {
    HMRDP_LOGI("ohaudio: workgroup created");
  }
  return true;
}

void OhAudioStop() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_ring.store(nullptr);
  if (g_renderer != nullptr) {
    OH_AudioRenderer_Stop(g_renderer);
    OH_AudioRenderer_Release(g_renderer);
    g_renderer = nullptr;
  }
  ReleaseWorkgroupLocked();
  g_resourceManager = nullptr;
}

void OhAudioWorkgroupBegin() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_workgroup == nullptr) {
    return;
  }
  if (g_threadState == 0) {
    if (OH_AudioWorkgroup_AddCurrentThread(g_workgroup, &g_workgroupToken)
        != AUDIOCOMMON_RESULT_SUCCESS) {
      g_workgroupToken = 0;
      g_threadState = 2;
      HMRDP_LOGW("ohaudio: workgroup add thread failed; playing without it");
      return;
    }
    g_threadState = 1;
    HMRDP_LOGI("ohaudio: workgroup thread registered");
  }
  if (g_threadState != 1) {
    return;
  }
  const uint64_t now = NowMs();
  OH_AudioWorkgroup_Start(g_workgroup, now, now + kWorkgroupDeadlineMs);
}

void OhAudioWorkgroupEnd() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_workgroup != nullptr && g_threadState == 1) {
    OH_AudioWorkgroup_Stop(g_workgroup);
  }
}

}  // namespace hmrdp
