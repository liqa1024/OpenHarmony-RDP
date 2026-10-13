/*
 * HmRdp - HarmonyOS OpenSL ES audio output backend for FreeRDP's rdpsnd.
 * OHOS exposes only SL_IID_OH_BUFFERQUEUE; the standard SL_IID_BUFFERQUEUE is unimplemented (silent), so swap the type.
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
		SLObjectItf engineObject;
		SLEngineItf engineEngine;

		SLObjectItf outputMixObject;

		SLObjectItf bqPlayerObject;
		SLPlayItf bqPlayerPlay;
		SLVolumeItf bqPlayerVolume;
		SLOHBufferQueueItf bqPlayerBufferQueue;

		unsigned int outchannels;
		unsigned int sr;
		unsigned int queuesize;

		// Pending 16-bit interleaved PCM awaiting the audio callback.
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
