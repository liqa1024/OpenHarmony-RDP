/*
 * HmRdp - HarmonyOS rdpsnd backend: hands decoded PCM to a sink registered by libhmrdp
 * (HmrdpSetAudioSink) instead of opening an audio device; the sink owns all client-side buffering.
 */
#include <freerdp/config.h>

#include <winpr/assert.h>
#include <winpr/crt.h>

#include <freerdp/types.h>
#include <freerdp/channels/log.h>

#include "rdpsnd_main.h"

typedef int (*HmrdpAudioSink)(void* context, const void* data, size_t size, int sampleRate,
                              int channels);

static HmrdpAudioSink g_audioSink = NULL;

FREERDP_API void HmrdpSetAudioSink(HmrdpAudioSink sink)
{
	g_audioSink = sink;
}

typedef struct
{
	rdpsndDevicePlugin device;
	UINT32 rate;
	UINT32 channels;
	UINT32 latency;
} rdpsndHmrdpPlugin;

static BOOL rdpsnd_hmrdp_format_supported(rdpsndDevicePlugin* device, const AUDIO_FORMAT* format)
{
	WINPR_ASSERT(device);
	WINPR_ASSERT(format);

	// Only advertise 16-bit PCM: FreeRDP decodes anything else to 16-bit before Play.
	if (format->wFormatTag != WAVE_FORMAT_PCM)
		return FALSE;
	if (format->cbSize != 0)
		return FALSE;
	if (format->wBitsPerSample != 16)
		return FALSE;
	if ((format->nChannels != 1) && (format->nChannels != 2))
		return FALSE;
	if (format->nSamplesPerSec > 48000)
		return FALSE;
	return TRUE;
}

static BOOL rdpsnd_hmrdp_open(rdpsndDevicePlugin* device, const AUDIO_FORMAT* format,
                              UINT32 latency)
{
	rdpsndHmrdpPlugin* plugin = (rdpsndHmrdpPlugin*)device;

	if (format)
	{
		plugin->rate = format->nSamplesPerSec;
		plugin->channels = format->nChannels;
	}
	plugin->latency = latency;
	return TRUE;
}

static UINT rdpsnd_hmrdp_play(rdpsndDevicePlugin* device, const BYTE* data, size_t size)
{
	rdpsndHmrdpPlugin* plugin = (rdpsndHmrdpPlugin*)device;
	UINT latency = 0;

	if ((g_audioSink != NULL) && (data != NULL) && (size > 0))
	{
		rdpContext* context = freerdp_rdpsnd_get_context(device->rdpsnd);

		/* The sink answers with the playback time it now holds (ms); rdpsnd adds it to the Wave Confirm. */
		latency = (UINT)g_audioSink(context, data, size, (int)plugin->rate, (int)plugin->channels);
	}
	return latency;
}

static void rdpsnd_hmrdp_start(rdpsndDevicePlugin* device)
{
	WINPR_UNUSED(device);
}

static void rdpsnd_hmrdp_close(rdpsndDevicePlugin* device)
{
	WINPR_UNUSED(device);
}

static void rdpsnd_hmrdp_free(rdpsndDevicePlugin* device)
{
	free(device);
}

static UINT32 rdpsnd_hmrdp_get_volume(rdpsndDevicePlugin* device)
{
	WINPR_UNUSED(device);
	return 0;
}

static BOOL rdpsnd_hmrdp_set_volume(rdpsndDevicePlugin* device, UINT32 value)
{
	WINPR_UNUSED(device);
	WINPR_UNUSED(value);
	return TRUE;
}

FREERDP_ENTRY_POINT(UINT VCAPITYPE opensles_freerdp_rdpsnd_client_subsystem_entry(
    PFREERDP_RDPSND_DEVICE_ENTRY_POINTS pEntryPoints))
{
	rdpsndHmrdpPlugin* plugin = (rdpsndHmrdpPlugin*)calloc(1, sizeof(rdpsndHmrdpPlugin));

	if (!plugin)
		return CHANNEL_RC_NO_MEMORY;

	plugin->device.Open = rdpsnd_hmrdp_open;
	plugin->device.FormatSupported = rdpsnd_hmrdp_format_supported;
	plugin->device.GetVolume = rdpsnd_hmrdp_get_volume;
	plugin->device.SetVolume = rdpsnd_hmrdp_set_volume;
	plugin->device.Start = rdpsnd_hmrdp_start;
	plugin->device.Play = rdpsnd_hmrdp_play;
	plugin->device.Close = rdpsnd_hmrdp_close;
	plugin->device.Free = rdpsnd_hmrdp_free;
	pEntryPoints->pRegisterRdpsndDevice(pEntryPoints->rdpsnd, (rdpsndDevicePlugin*)plugin);
	return CHANNEL_RC_OK;
}
