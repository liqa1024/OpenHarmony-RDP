/*
 * HmRdp - HarmonyOS OpenSL ES audio output backend for FreeRDP's rdpsnd.
 *
 * OHOS ships an OpenSL ES 1.0.1 implementation that only exposes the
 * OH-specific buffer queue (SL_IID_OH_BUFFERQUEUE); the standard
 * SL_IID_BUFFERQUEUE used by upstream FreeRDP is not implemented, so
 * CreateAudioPlayer succeeds but no interface can be obtained and playback is
 * silent. This header mirrors the upstream API but swaps the buffer queue type
 * for the OH one.
 */
#ifndef FREERDP_CHANNEL_RDPSND_CLIENT_OPENSL_IO_H
#define FREERDP_CHANNEL_RDPSND_CLIENT_OPENSL_IO_H

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_OpenHarmony.h>
#include <stdlib.h>
#include <winpr/synch.h>

#include <freerdp/api.h>

#ifdef __cplusplus
extern "C"
{
#endif

	typedef struct
	{
		// engine interfaces
		SLObjectItf engineObject;
		SLEngineItf engineEngine;

		// output mix interfaces
		SLObjectItf outputMixObject;

		// buffer queue player interfaces
		SLObjectItf bqPlayerObject;
		SLPlayItf bqPlayerPlay;
		SLVolumeItf bqPlayerVolume;
		SLOHBufferQueueItf bqPlayerBufferQueue;

		unsigned int outchannels;
		unsigned int sr;
		unsigned int queuesize;

		// Pending PCM data (16-bit interleaved) waiting for the audio thread to
		// pull it through the OH buffer queue callback.
		BYTE* pending;
		size_t pendingCapacity;
		size_t pendingHead;
		size_t pendingTail;
		size_t pendingCount;
		CRITICAL_SECTION lock;
	} OPENSL_STREAM;

	FREERDP_LOCAL OPENSL_STREAM* android_OpenAudioDevice(int sr, int outchannels, int bufferframes);
	FREERDP_LOCAL void android_CloseAudioDevice(OPENSL_STREAM* p);
	FREERDP_LOCAL int android_AudioOut(OPENSL_STREAM* p, const short* buffer, int size);
	FREERDP_LOCAL int android_GetOutputMute(OPENSL_STREAM* p);
	FREERDP_LOCAL BOOL android_SetOutputMute(OPENSL_STREAM* p, BOOL mute);
	FREERDP_LOCAL int android_GetOutputVolume(OPENSL_STREAM* p);
	FREERDP_LOCAL int android_GetOutputVolumeMax(OPENSL_STREAM* p);
	FREERDP_LOCAL BOOL android_SetOutputVolume(OPENSL_STREAM* p, int level);
#ifdef __cplusplus
};
#endif

#endif /* FREERDP_CHANNEL_RDPSND_CLIENT_OPENSL_IO_H */
