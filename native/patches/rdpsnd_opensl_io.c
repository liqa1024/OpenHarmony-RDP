/*
 * HmRdp - HarmonyOS OpenSL ES audio output backend for FreeRDP's rdpsnd.
 *
 * See rdpsnd_opensl_io.h for why the OH buffer queue is used instead of the
 * standard one. The player pulls data: the OH callback hands us a buffer to
 * fill, and android_AudioOut only appends PCM into a bounded ring buffer that
 * the callback drains. All access to the ring is serialized by a lock because
 * the two sides run on different threads.
 */
#include <winpr/assert.h>
#include <winpr/crt.h>

#include "rdpsnd_main.h"
#include "opensl_io.h"

// Keep at most ~1s of 48kHz stereo 16-bit audio; drop the oldest data if the
// server outruns playback so latency never grows without bound.
#define HMRDP_PCM_RING_CAPACITY (512 * 1024)

static void bqPlayerCallback(SLOHBufferQueueItf bq, void* context, SLuint32 size);

static SLresult openSLCreateEngine(OPENSL_STREAM* p)
{
	SLresult result;

	result = slCreateEngine(&(p->engineObject), 0, NULL, 0, NULL, NULL);
	if (result != SL_RESULT_SUCCESS)
		return result;

	result = (*p->engineObject)->Realize(p->engineObject, SL_BOOLEAN_FALSE);
	if (result != SL_RESULT_SUCCESS)
		return result;

	result = (*p->engineObject)->GetInterface(p->engineObject, SL_IID_ENGINE, &(p->engineEngine));
	return result;
}

static SLresult openSLPlayOpen(OPENSL_STREAM* p)
{
	SLresult result;
	SLuint32 sr = p->sr;
	SLuint32 channels = p->outchannels;

	if (channels == 0)
		return SL_RESULT_SUCCESS;

	switch (sr)
	{
		case 8000:
			sr = SL_SAMPLINGRATE_8;
			break;
		case 11025:
			sr = SL_SAMPLINGRATE_11_025;
			break;
		case 16000:
			sr = SL_SAMPLINGRATE_16;
			break;
		case 22050:
			sr = SL_SAMPLINGRATE_22_05;
			break;
		case 24000:
			sr = SL_SAMPLINGRATE_24;
			break;
		case 32000:
			sr = SL_SAMPLINGRATE_32;
			break;
		case 44100:
			sr = SL_SAMPLINGRATE_44_1;
			break;
		case 48000:
			sr = SL_SAMPLINGRATE_48;
			break;
		default:
			return (SLresult)-1;
	}

	result = (*p->engineEngine)
	             ->CreateOutputMix(p->engineEngine, &(p->outputMixObject), 0, NULL, NULL);
	if (result != SL_RESULT_SUCCESS)
		goto out;

	result = (*p->outputMixObject)->Realize(p->outputMixObject, SL_BOOLEAN_FALSE);
	if (result != SL_RESULT_SUCCESS)
		goto out;

	const int speakers = (channels > 1) ? (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT)
	                                    : SL_SPEAKER_FRONT_CENTER;
	const SLDataLocator_BufferQueue loc_bufq = { SL_DATALOCATOR_BUFFERQUEUE, p->queuesize };
	const SLDataFormat_PCM format_pcm = { SL_DATAFORMAT_PCM, channels, sr,
		                                  SL_PCMSAMPLEFORMAT_FIXED_16,
		                                  SL_PCMSAMPLEFORMAT_FIXED_16, speakers,
		                                  SL_BYTEORDER_LITTLEENDIAN };
	SLDataSource audioSrc = { (void*)&loc_bufq, (void*)&format_pcm };
	const SLDataLocator_OutputMix loc_outmix = { SL_DATALOCATOR_OUTPUTMIX, p->outputMixObject };
	SLDataSink audioSnk = { (void*)&loc_outmix, NULL };

	// OHOS only supports the OH buffer queue, PLAY and VOLUME interfaces. Do
	// not request any interface at creation time; fetch them below instead.
	result = (*p->engineEngine)
	             ->CreateAudioPlayer(p->engineEngine, &(p->bqPlayerObject), &audioSrc, &audioSnk,
	                                 0, NULL, NULL);
	if (result != SL_RESULT_SUCCESS)
		goto out;

	result = (*p->bqPlayerObject)->Realize(p->bqPlayerObject, SL_BOOLEAN_FALSE);
	if (result != SL_RESULT_SUCCESS)
		goto out;

	result = (*p->bqPlayerObject)
	             ->GetInterface(p->bqPlayerObject, SL_IID_PLAY, &(p->bqPlayerPlay));
	if (result != SL_RESULT_SUCCESS)
		goto out;

	// Volume is optional: ignore failure so playback still works without it.
	(void)(*p->bqPlayerObject)
	    ->GetInterface(p->bqPlayerObject, SL_IID_VOLUME, &(p->bqPlayerVolume));

	result = (*p->bqPlayerObject)
	             ->GetInterface(p->bqPlayerObject, SL_IID_OH_BUFFERQUEUE,
	                            &(p->bqPlayerBufferQueue));
	if (result != SL_RESULT_SUCCESS)
		goto out;

	result = (*p->bqPlayerBufferQueue)
	             ->RegisterCallback(p->bqPlayerBufferQueue, bqPlayerCallback, p);
	if (result != SL_RESULT_SUCCESS)
		goto out;

	result = (*p->bqPlayerPlay)->SetPlayState(p->bqPlayerPlay, SL_PLAYSTATE_PLAYING);
out:
	return result;
}

static void openSLDestroyEngine(OPENSL_STREAM* p)
{
	if (p->bqPlayerObject != NULL)
	{
		(*p->bqPlayerObject)->Destroy(p->bqPlayerObject);
		p->bqPlayerObject = NULL;
		p->bqPlayerVolume = NULL;
		p->bqPlayerPlay = NULL;
		p->bqPlayerBufferQueue = NULL;
	}

	if (p->outputMixObject != NULL)
	{
		(*p->outputMixObject)->Destroy(p->outputMixObject);
		p->outputMixObject = NULL;
	}

	if (p->engineObject != NULL)
	{
		(*p->engineObject)->Destroy(p->engineObject);
		p->engineObject = NULL;
		p->engineEngine = NULL;
	}
}

OPENSL_STREAM* android_OpenAudioDevice(int sr, int outchannels, int bufferframes)
{
	OPENSL_STREAM* p = (OPENSL_STREAM*)calloc(1, sizeof(OPENSL_STREAM));

	if (!p)
		return NULL;

	p->queuesize = bufferframes > 0 ? (unsigned int)bufferframes : 1;
	p->outchannels = outchannels;
	p->sr = sr;
	p->pendingCapacity = HMRDP_PCM_RING_CAPACITY;
	p->pending = (BYTE*)calloc(1, p->pendingCapacity);
	InitializeCriticalSection(&p->lock);

	if (!p->pending)
	{
		DeleteCriticalSection(&p->lock);
		free(p);
		return NULL;
	}

	if (openSLCreateEngine(p) != SL_RESULT_SUCCESS)
	{
		android_CloseAudioDevice(p);
		return NULL;
	}

	if (openSLPlayOpen(p) != SL_RESULT_SUCCESS)
	{
		android_CloseAudioDevice(p);
		return NULL;
	}

	return p;
}

void android_CloseAudioDevice(OPENSL_STREAM* p)
{
	if (p == NULL)
		return;

	openSLDestroyEngine(p);
	DeleteCriticalSection(&p->lock);
	free(p->pending);
	free(p);
}

// Invoked on the audio thread when the player needs more data. Copy whatever
// PCM is pending (padding with silence on underrun) into the OH buffer.
static void bqPlayerCallback(SLOHBufferQueueItf bq, void* context, SLuint32 size)
{
	OPENSL_STREAM* p = (OPENSL_STREAM*)context;
	SLuint8* buffer = NULL;
	SLuint32 bufferSize = 0;

	if (!p || !bq)
		return;

	if ((*bq)->GetBuffer(bq, &buffer, &bufferSize) != SL_RESULT_SUCCESS || !buffer)
		return;

	SLuint32 want = size;
	if ((want == 0) || (want > bufferSize))
		want = bufferSize;
	if (want == 0)
		return;

	EnterCriticalSection(&p->lock);

	size_t available = p->pendingCount;
	size_t copied = (available < want) ? available : want;
	size_t remaining = copied;
	size_t pos = 0;

	while (remaining > 0)
	{
		const size_t first = p->pendingCapacity - p->pendingHead;
		const size_t chunk = (first < remaining) ? first : remaining;
		CopyMemory(buffer + pos, p->pending + p->pendingHead, chunk);
		p->pendingHead = (p->pendingHead + chunk) % p->pendingCapacity;
		p->pendingCount -= chunk;
		pos += chunk;
		remaining -= chunk;
	}

	LeaveCriticalSection(&p->lock);

	if (copied < want)
		ZeroMemory(buffer + copied, want - copied);

	(void)(*bq)->Enqueue(bq, buffer, want);
}

int android_AudioOut(OPENSL_STREAM* p, const short* buffer, int size)
{
	WINPR_ASSERT(p);
	WINPR_ASSERT(buffer);
	WINPR_ASSERT(size > 0);

	if (!p || !p->pending || !buffer || (size <= 0))
		return -1;

	const BYTE* src = (const BYTE*)buffer;
	size_t bytes = (size_t)size * sizeof(short);

	EnterCriticalSection(&p->lock);

	// Drop the oldest data when full so we never block the RDP thread.
	if (bytes > p->pendingCapacity)
	{
		src += bytes - p->pendingCapacity;
		bytes = p->pendingCapacity;
	}
	while ((p->pendingCount + bytes) > p->pendingCapacity && p->pendingCount > 0)
	{
		p->pendingHead = (p->pendingHead + 1) % p->pendingCapacity;
		p->pendingCount--;
	}

	size_t remaining = bytes;
	size_t pos = 0;
	while (remaining > 0)
	{
		const size_t first = p->pendingCapacity - p->pendingTail;
		const size_t chunk = (first < remaining) ? first : remaining;
		CopyMemory(p->pending + p->pendingTail, src + pos, chunk);
		p->pendingTail = (p->pendingTail + chunk) % p->pendingCapacity;
		p->pendingCount += chunk;
		pos += chunk;
		remaining -= chunk;
	}

	LeaveCriticalSection(&p->lock);

	return size;
}

int android_GetOutputMute(OPENSL_STREAM* p)
{
	SLboolean mute = SL_BOOLEAN_FALSE;

	if (!p || !p->bqPlayerVolume)
		return 0;

	if ((*p->bqPlayerVolume)->GetMute(p->bqPlayerVolume, &mute) != SL_RESULT_SUCCESS)
		return 0;

	return mute;
}

BOOL android_SetOutputMute(OPENSL_STREAM* p, BOOL mute)
{
	if (!p || !p->bqPlayerVolume)
		return TRUE;

	// OHOS does not implement the mute interface; treat it as best effort.
	(void)(*p->bqPlayerVolume)->SetMute(p->bqPlayerVolume, mute ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE);
	return TRUE;
}

int android_GetOutputVolume(OPENSL_STREAM* p)
{
	SLmillibel level = SL_MILLIBEL_MIN;

	if (!p || !p->bqPlayerVolume)
		return 0;

	if ((*p->bqPlayerVolume)->GetVolumeLevel(p->bqPlayerVolume, &level) != SL_RESULT_SUCCESS)
		return 0;

	return level;
}

int android_GetOutputVolumeMax(OPENSL_STREAM* p)
{
	SLmillibel level = SL_MILLIBEL_MAX;

	if (!p || !p->bqPlayerVolume)
		return SL_MILLIBEL_MAX;

	if ((*p->bqPlayerVolume)->GetMaxVolumeLevel(p->bqPlayerVolume, &level) != SL_RESULT_SUCCESS)
		return SL_MILLIBEL_MAX;

	return level;
}

BOOL android_SetOutputVolume(OPENSL_STREAM* p, int level)
{
	if (!p || !p->bqPlayerVolume)
		return FALSE;

	if ((*p->bqPlayerVolume)->SetVolumeLevel(p->bqPlayerVolume, level) != SL_RESULT_SUCCESS)
		return FALSE;

	return TRUE;
}
