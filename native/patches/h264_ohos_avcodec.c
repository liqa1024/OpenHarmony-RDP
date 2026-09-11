/*
 * HmRdp - HarmonyOS AVCodec (OH_VideoDecoder) backed H.264 decoder subsystem.
 *
 * FreeRDP ships pluggable H.264 subsystems (libfreerdp/codec/h264.h); upstream
 * only provides ffmpeg/openh264/MediaCodec/MediaFoundation. On HarmonyOS none
 * of those is available, so AVC420/AVC444 was never advertised and the server
 * always fell back to RemoteFX Progressive. This subsystem decodes the RDP
 * H.264 access unit with the OHOS video decoder in synchronous buffer mode and
 * hands the resulting I420 planes back to FreeRDP, which keeps doing the
 * YUV->RGB conversion and compositing exactly as before.
 *
 * Subsystem contract (same as h264_ffmpeg/mediacodec): on success Decompress
 * points h264->pYUVData[x] at the decoded planes and updates h264->iStride[x].
 * The planes live in this subsystem's own buffers, because the OHOS output
 * buffer is released before Decompress returns.
 *
 * This is the "hardware decode" step: the H.264 entropy decode (and, when the
 * device has one, the whole decode) runs in the platform decoder instead of on
 * the CPU. The decoded frame is copied into these YUV buffers, so this is not
 * yet zero-copy - see PERF-TODO.md for the OH_NativeImage/OES follow-up.
 *
 * AVC420 and AVC444 share the same Decompress path: for AVC444 FreeRDP calls
 * it once per bitstream and combines the two 4:2:0 frames itself.
 */
#include <freerdp/config.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <winpr/assert.h>
#include <winpr/crt.h>
#include <winpr/wlog.h>

#include <freerdp/codec/h264.h>

#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avbuffer_info.h>
#include <multimedia/player_framework/native_avcapability.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_averrors.h>
#include <multimedia/player_framework/native_avformat.h>

#include "h264.h"

#define TAG FREERDP_TAG("codec")

/* Blocking timeout per Query*Buffer call; RDP frames are produced in order and
 * screen content has no B-frame reordering, so a decoded frame is normally
 * ready immediately. */
#define OHOS_DEC_TIMEOUT_US (50 * 1000)
#define OHOS_DEC_MAX_RETRY 20

static INLINE UINT32 align16(UINT32 value)
{
	return (value + 15u) & ~15u;
}

/* User setting: prefer a hardware H.264 decoder (VDEC) over a software one.
 * Read by the decoder factory; set from libhmrdp before a session connects. */
static int g_hardwarePreferred = 1;

void HmrdpH264SetHardwarePreferred(int preferred)
{
	g_hardwarePreferred = preferred ? 1 : 0;
}

static int32_t ohos_format_int(OH_AVFormat* format, const char* key, int32_t fallback);

typedef struct
{
	OH_AVCodec* codec;
	int32_t width;
	int32_t height;
	int64_t pts;
	/* Set when the decoder only offers NV12 (interleaved UV). */
	BOOL nv12;
	BOOL hardware;
	BOOL logged;
	BOOL loggedInput;
	/* At least one frame has been decoded into planes. */
	BOOL hasFrame;
	/* Decoded frame counter, used for a periodic "H.264 active" log. */
	uint32_t decodedFrames;
	/* Planar destination handed back to FreeRDP via h264->pYUVData/iStride. */
	BYTE* planes[3];
	UINT32 stride[3];
	UINT32 planeSize[3];
	int32_t bufWidth;
	int32_t bufHeight;
} H264_CONTEXT_OHOS_AVCODEC;

static void ohos_avcodec_free_planes(H264_CONTEXT_OHOS_AVCODEC* sys)
{
	if (!sys)
		return;
	for (size_t i = 0; i < 3; i++)
	{
		winpr_aligned_free(sys->planes[i]);
		sys->planes[i] = NULL;
		sys->stride[i] = 0;
		sys->planeSize[i] = 0;
	}
	sys->bufWidth = 0;
	sys->bufHeight = 0;
}

static BOOL ohos_avcodec_alloc_planes(H264_CONTEXT_OHOS_AVCODEC* sys, UINT32 width, UINT32 height)
{
	if (sys->bufWidth == (int32_t)width && sys->bufHeight == (int32_t)height && sys->planes[0])
		return TRUE;

	ohos_avcodec_free_planes(sys);

	const UINT32 strideY = align16(width);
	const UINT32 planeH = align16(height);
	const UINT32 strideUV = (strideY + 1u) / 2u;
	const UINT32 planeHUV = (planeH + 1u) / 2u;

	sys->stride[0] = strideY;
	sys->stride[1] = strideUV;
	sys->stride[2] = strideUV;
	sys->planeSize[0] = strideY * planeH;
	sys->planeSize[1] = strideUV * planeHUV;
	sys->planeSize[2] = strideUV * planeHUV;

	for (size_t i = 0; i < 3; i++)
	{
		sys->planes[i] = (BYTE*)winpr_aligned_recalloc(NULL, sys->planeSize[i], 1, 16);
		if (!sys->planes[i])
		{
			ohos_avcodec_free_planes(sys);
			return FALSE;
		}
	}
	sys->bufWidth = (int32_t)width;
	sys->bufHeight = (int32_t)height;
	return TRUE;
}

static void ohos_avcodec_close(H264_CONTEXT_OHOS_AVCODEC* sys)
{
	if (!sys)
		return;
	if (sys->codec)
	{
		OH_VideoDecoder_Stop(sys->codec);
		OH_VideoDecoder_Destroy(sys->codec);
		sys->codec = NULL;
	}
	ohos_avcodec_free_planes(sys);
	sys->hasFrame = FALSE;
	sys->logged = FALSE;
	sys->loggedInput = FALSE;
	sys->decodedFrames = 0;
}

/* Prefer an explicitly hardware-accelerated decoder (OH_VideoDecoder_CreateByMime
 * may hand back a software one); fall back to the default codec if none. */
static OH_AVCodec* ohos_avcodec_create(H264_CONTEXT* h264, BOOL* hardware, const char** name)
{
	*hardware = FALSE;
	*name = NULL;

	const OH_AVCodecCategory category = g_hardwarePreferred ? HARDWARE : SOFTWARE;
	OH_AVCapability* cap = OH_AVCodec_GetCapabilityByCategory(OH_AVCODEC_MIMETYPE_VIDEO_AVC,
	                                                          false, category);
	if (cap == NULL && !g_hardwarePreferred)
	{
		/* No software decoder on this device: fall back to the default one
		 * rather than failing H.264 entirely. */
		cap = OH_AVCodec_GetCapability(OH_AVCODEC_MIMETYPE_VIDEO_AVC, false);
	}
	if (cap != NULL)
	{
		OH_AVCodec* codec = NULL;
		const char* capName = OH_AVCapability_GetName(cap);
		if (capName != NULL)
			codec = OH_VideoDecoder_CreateByName(capName);
		if (codec != NULL)
		{
			*hardware = OH_AVCapability_IsHardware(cap);
			*name = capName;
			return codec;
		}
	}

	OH_AVCodec* codec = OH_VideoDecoder_CreateByMime(OH_AVCODEC_MIMETYPE_VIDEO_AVC);
	if (codec != NULL)
		*name = "by-mime";
	return codec;
}

static BOOL ohos_avcodec_configure(H264_CONTEXT* h264, H264_CONTEXT_OHOS_AVCODEC* sys,
                                   int32_t width, int32_t height, int32_t pixelFormat)
{
	BOOL hardware = FALSE;
	const char* name = NULL;
	OH_AVCodec* codec = ohos_avcodec_create(h264, &hardware, &name);
	if (!codec)
	{
		WLog_Print(h264->log, WLOG_ERROR, "no H.264 video decoder available");
		return FALSE;
	}
	sys->hardware = hardware;
	WLog_Print(h264->log, WLOG_INFO, "OHOS H.264 decoder: %s (hardware=%d)",
	           name != NULL ? name : "?", hardware ? 1 : 0);

	OH_AVFormat* format = OH_AVFormat_Create();
	if (!format)
	{
		OH_VideoDecoder_Destroy(codec);
		return FALSE;
	}
	OH_AVFormat_SetIntValue(format, OH_MD_KEY_WIDTH, width);
	OH_AVFormat_SetIntValue(format, OH_MD_KEY_HEIGHT, height);
	/* Ask for the pixel format only when the caller picked one; leaving it unset
	 * lets the decoder use its native output (usually NV12). */
	if (pixelFormat > 0)
		OH_AVFormat_SetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, pixelFormat);
	OH_AVFormat_SetIntValue(format, OH_MD_KEY_VIDEO_ENABLE_LOW_LATENCY, 1);
	/* Required by the SDK to enter the synchronous Query*Buffer mode. */
	OH_AVFormat_SetIntValue(format, OH_MD_KEY_ENABLE_SYNC_MODE, 1);
	const OH_AVErrCode cret = OH_VideoDecoder_Configure(codec, format);
	OH_AVFormat_Destroy(format);
	if (cret != AV_ERR_OK)
	{
		OH_VideoDecoder_Destroy(codec);
		WLog_Print(h264->log, WLOG_WARN,
		           "OH_VideoDecoder_Configure failed (%d) for %" PRId32 "x%" PRId32
		           " fmt=%" PRId32,
		           (int)cret, width, height, pixelFormat);
		return FALSE;
	}

	OH_AVErrCode ret = OH_VideoDecoder_Prepare(codec);
	if (ret == AV_ERR_OK)
		ret = OH_VideoDecoder_Start(codec);
	if (ret != AV_ERR_OK)
	{
		OH_VideoDecoder_Destroy(codec);
		WLog_Print(h264->log, WLOG_WARN, "OH_VideoDecoder start failed (%d)", (int)ret);
		return FALSE;
	}

	sys->codec = codec;
	sys->width = width;
	sys->height = height;
	sys->pts = 0;
	/* NV12 is the common buffer-mode output; refined from the description. */
	sys->nv12 = TRUE;

	{
		OH_AVFormat* desc = OH_VideoDecoder_GetOutputDescription(codec);
		const int32_t dw = ohos_format_int(desc, OH_MD_KEY_VIDEO_PIC_WIDTH, -1);
		const int32_t dh = ohos_format_int(desc, OH_MD_KEY_VIDEO_PIC_HEIGHT, -1);
		const int32_t ds = ohos_format_int(desc, OH_MD_KEY_VIDEO_STRIDE, -1);
		const int32_t dsh = ohos_format_int(desc, OH_MD_KEY_VIDEO_SLICE_HEIGHT, -1);
		const int32_t dfmt = ohos_format_int(desc, OH_MD_KEY_PIXEL_FORMAT, -1);
		if (desc)
			OH_AVFormat_Destroy(desc);
		if (dfmt == AV_PIXEL_FORMAT_YUVI420)
			sys->nv12 = FALSE;
		WLog_Print(h264->log, WLOG_INFO,
		           "OHOS decoder outdesc pic=%" PRId32 "x%" PRId32 " stride=%" PRId32
		           " slice=%" PRId32 " fmt=%" PRId32 " (%s)",
		           dw, dh, ds, dsh, dfmt, sys->nv12 ? "NV12" : "I420");
	}
	return TRUE;
}

static BOOL ohos_avcodec_open(H264_CONTEXT* h264, H264_CONTEXT_OHOS_AVCODEC* sys, int32_t width,
                              int32_t height)
{
	ohos_avcodec_close(sys);
	/* Let the decoder pick its native output format (0 = unset). */
	if (ohos_avcodec_configure(h264, sys, width, height, 0))
		return TRUE;
	/* Fallbacks for decoders that require an explicit format. */
	if (ohos_avcodec_configure(h264, sys, width, height, AV_PIXEL_FORMAT_NV12))
		return TRUE;
	return ohos_avcodec_configure(h264, sys, width, height, AV_PIXEL_FORMAT_YUVI420);
}

/* The decoder needs to be told which access units carry parameter sets and
 * which are key frames, otherwise it discards frames until it sees an
 * (XPS + key frame) pair. Scan the Annex-B NAL units for SPS(7)/PPS(8)/IDR(5). */
static uint32_t ohos_scan_flags(const BYTE* data, UINT32 size)
{
	uint32_t flags = AVCODEC_BUFFER_FLAGS_NONE;
	UINT32 i = 0;
	while (i + 3 <= size)
	{
		UINT32 nal = 0;
		if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
			nal = i + 3;
		else if ((i + 4 <= size) && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 &&
		         data[i + 3] == 1)
			nal = i + 4;
		else
		{
			i++;
			continue;
		}
		if (nal < size)
		{
			const uint8_t type = data[nal] & 0x1Fu;
			if (type == 7 || type == 8)
				flags |= AVCODEC_BUFFER_FLAGS_CODEC_DATA;
			if (type == 5)
				flags |= AVCODEC_BUFFER_FLAGS_SYNC_FRAME;
		}
		i = nal + 1;
	}
	return flags;
}

static BOOL ohos_avcodec_push(H264_CONTEXT* h264, H264_CONTEXT_OHOS_AVCODEC* sys,
                              const BYTE* data, UINT32 size)
{
	OH_AVErrCode last = AV_ERR_OK;
	for (int attempt = 0; attempt < OHOS_DEC_MAX_RETRY; ++attempt)
	{
		uint32_t index = 0;
		const OH_AVErrCode qret =
		    OH_VideoDecoder_QueryInputBuffer(sys->codec, &index, OHOS_DEC_TIMEOUT_US);
		if (qret == AV_ERR_TRY_AGAIN_LATER)
		{
			last = qret;
			continue;
		}
		if (qret != AV_ERR_OK)
		{
			WLog_Print(h264->log, WLOG_WARN, "QueryInputBuffer failed (%d)", (int)qret);
			return FALSE;
		}

		OH_AVBuffer* buffer = OH_VideoDecoder_GetInputBuffer(sys->codec, index);
		if (!buffer)
		{
			WLog_Print(h264->log, WLOG_WARN, "GetInputBuffer null");
			return FALSE;
		}
		uint8_t* addr = OH_AVBuffer_GetAddr(buffer);
		const int32_t capacity = OH_AVBuffer_GetCapacity(buffer);
		if (!addr || capacity <= 0 || (int32_t)size > capacity)
		{
			WLog_Print(h264->log, WLOG_WARN, "input buffer unusable cap=%" PRId32 " need=%" PRIu32,
			           capacity, size);
			return FALSE;
		}
		memcpy(addr, data, size);

		OH_AVCodecBufferAttr attr;
		memset(&attr, 0, sizeof(attr));
		attr.pts = sys->pts++;
		attr.size = (int32_t)size;
		attr.offset = 0;
		attr.flags = ohos_scan_flags(data, size);
		const OH_AVErrCode sret = OH_AVBuffer_SetBufferAttr(buffer, &attr);
		if (sret != AV_ERR_OK)
		{
			WLog_Print(h264->log, WLOG_WARN, "SetBufferAttr failed (%d)", (int)sret);
			return FALSE;
		}
		const OH_AVErrCode pret = OH_VideoDecoder_PushInputBuffer(sys->codec, index);
		if (pret != AV_ERR_OK)
		{
			WLog_Print(h264->log, WLOG_WARN, "PushInputBuffer failed (%d)", (int)pret);
			return FALSE;
		}
		return TRUE;
	}
	WLog_Print(h264->log, WLOG_WARN, "QueryInputBuffer exhausted (last=%d)", (int)last);
	return FALSE;
}

static int32_t ohos_format_int(OH_AVFormat* format, const char* key, int32_t fallback)
{
	int32_t value = fallback;
	if (format)
		(void)OH_AVFormat_GetIntValue(format, key, &value);
	return value;
}

static BOOL ohos_avcodec_copy_yuv(H264_CONTEXT* h264, H264_CONTEXT_OHOS_AVCODEC* sys,
                                  OH_AVBuffer* buffer)
{
	OH_AVFormat* desc = OH_VideoDecoder_GetOutputDescription(sys->codec);
	int32_t stride = ohos_format_int(desc, OH_MD_KEY_VIDEO_STRIDE, 0);
	int32_t sliceHeight = ohos_format_int(desc, OH_MD_KEY_VIDEO_SLICE_HEIGHT, 0);
	const int32_t picWidth = ohos_format_int(desc, OH_MD_KEY_VIDEO_PIC_WIDTH, (int32_t)h264->width);
	const int32_t picHeight =
	    ohos_format_int(desc, OH_MD_KEY_VIDEO_PIC_HEIGHT, (int32_t)h264->height);
	if (desc)
		OH_AVFormat_Destroy(desc);
	if (stride <= 0)
		stride = picWidth;
	if (sliceHeight <= 0)
		sliceHeight = picHeight;

	const uint8_t* base = OH_AVBuffer_GetAddr(buffer);
	if (!base)
		return FALSE;

	const int32_t visibleW = picWidth < (int32_t)h264->width ? picWidth : (int32_t)h264->width;
	const int32_t visibleH = picHeight < (int32_t)h264->height ? picHeight : (int32_t)h264->height;
	if (visibleW <= 0 || visibleH <= 0)
		return FALSE;

	if (!ohos_avcodec_alloc_planes(sys, (UINT32)visibleW, (UINT32)visibleH))
		return FALSE;

	/* Luma is plane 0 in both I420 and NV12; the visible width may be smaller
	 * than the destination stride, so copy row by row. */
	for (int32_t y = 0; y < visibleH; ++y)
		memcpy(sys->planes[0] + (size_t)y * sys->stride[0], base + (size_t)y * stride,
		       (size_t)visibleW);

	if (sys->nv12)
	{
		/* Interleaved UV plane follows the (padded) luma plane. */
		const uint8_t* uv = base + (size_t)stride * sliceHeight;
		for (int32_t y = 0; y < visibleH / 2; ++y)
		{
			uint8_t* du = sys->planes[1] + (size_t)y * sys->stride[1];
			uint8_t* dv = sys->planes[2] + (size_t)y * sys->stride[2];
			const uint8_t* suv = uv + (size_t)y * stride;
			for (int32_t x = 0; x < visibleW / 2; ++x)
			{
				du[x] = suv[2 * x];
				dv[x] = suv[2 * x + 1];
			}
		}
	}
	else
	{
		const uint8_t* su = base + (size_t)stride * sliceHeight;
		const uint8_t* sv = su + (size_t)(stride / 2) * (sliceHeight / 2);
		for (int32_t y = 0; y < visibleH / 2; ++y)
		{
			memcpy(sys->planes[1] + (size_t)y * sys->stride[1], su + (size_t)y * (stride / 2),
			       (size_t)(visibleW / 2));
			memcpy(sys->planes[2] + (size_t)y * sys->stride[2], sv + (size_t)y * (stride / 2),
			       (size_t)(visibleW / 2));
		}
	}

	/* Hand the planes back to FreeRDP (subsystem contract). */
	h264->pYUVData[0] = sys->planes[0];
	h264->pYUVData[1] = sys->planes[1];
	h264->pYUVData[2] = sys->planes[2];
	h264->iStride[0] = sys->stride[0];
	h264->iStride[1] = sys->stride[1];
	h264->iStride[2] = sys->stride[2];
	return TRUE;
}

static BOOL ohos_avcodec_pull(H264_CONTEXT* h264, H264_CONTEXT_OHOS_AVCODEC* sys)
{
	OH_AVErrCode last = AV_ERR_OK;
	for (int attempt = 0; attempt < OHOS_DEC_MAX_RETRY; ++attempt)
	{
		uint32_t index = 0;
		const OH_AVErrCode qret =
		    OH_VideoDecoder_QueryOutputBuffer(sys->codec, &index, OHOS_DEC_TIMEOUT_US);
		if (qret == AV_ERR_TRY_AGAIN_LATER)
		{
			last = qret;
			continue;
		}
		if (qret == AV_ERR_STREAM_CHANGED)
		{
			last = qret;
			continue; /* geometry is re-read from GetOutputDescription below */
		}
		if (qret != AV_ERR_OK)
		{
			WLog_Print(h264->log, WLOG_WARN, "QueryOutputBuffer failed (%d)", (int)qret);
			return FALSE;
		}

		OH_AVBuffer* buffer = OH_VideoDecoder_GetOutputBuffer(sys->codec, index);
		if (!buffer)
		{
			WLog_Print(h264->log, WLOG_WARN, "GetOutputBuffer null");
			return FALSE;
		}
		OH_AVCodecBufferAttr attr;
		memset(&attr, 0, sizeof(attr));
		if (OH_AVBuffer_GetBufferAttr(buffer, &attr) != AV_ERR_OK)
		{
			WLog_Print(h264->log, WLOG_WARN, "GetBufferAttr(output) failed");
			return FALSE;
		}

		if ((attr.flags & AVCODEC_BUFFER_FLAGS_EOS) != 0)
		{
			WLog_Print(h264->log, WLOG_WARN, "decoder EOS (size=%" PRId32 ")", attr.size);
			OH_VideoDecoder_FreeOutputBuffer(sys->codec, index);
			return FALSE;
		}
		const BOOL ok = ohos_avcodec_copy_yuv(h264, sys, buffer);
		OH_VideoDecoder_FreeOutputBuffer(sys->codec, index);
		if (!ok)
			WLog_Print(h264->log, WLOG_WARN, "copy_yuv failed");
		else if ((++sys->decodedFrames % 60u) == 0u)
			WLog_Print(h264->log, WLOG_INFO, "H.264 active: %" PRIu32 " frames decoded",
			           sys->decodedFrames);
		return ok;
	}
	WLog_Print(h264->log, WLOG_WARN, "QueryOutputBuffer exhausted (last=%d)", (int)last);
	return FALSE;
}

static int ohos_avcodec_decompress(H264_CONTEXT* WINPR_RESTRICT h264,
                                   const BYTE* WINPR_RESTRICT pSrcData, UINT32 SrcSize)
{
	H264_CONTEXT_OHOS_AVCODEC* sys = h264 ? (H264_CONTEXT_OHOS_AVCODEC*)h264->pSystemData : NULL;
	WINPR_ASSERT(h264);

	if (!sys || !pSrcData || SrcSize == 0)
		return -1;

	const int32_t width = (int32_t)h264->width;
	const int32_t height = (int32_t)h264->height;
	if (width <= 0 || height <= 0)
		return -1;

	if (!sys->loggedInput)
	{
		char hex[16 * 3 + 1];
		size_t n = 0;
		const size_t maxBytes = SrcSize < 16 ? SrcSize : 16;
		for (size_t i = 0; i < maxBytes; i++)
			n += (size_t)snprintf(hex + n, sizeof(hex) - n, "%02x ", pSrcData[i]);
		hex[n] = '\0';
		WLog_Print(h264->log, WLOG_INFO, "h264 AU size=%" PRIu32 " head=%s", SrcSize, hex);
		sys->loggedInput = TRUE;
	}

	if (!sys->codec || sys->width != width || sys->height != height)
	{
		if (!ohos_avcodec_open(h264, sys, width, height))
			return -1;
		if (!sys->logged)
		{
			WLog_Print(h264->log, WLOG_INFO,
			           "OHOS AVCodec H.264 decoder opened %" PRId32 "x%" PRId32 " (%s, %s)",
			           width, height, sys->nv12 ? "NV12" : "I420",
			           sys->hardware ? "hardware" : "software");
			sys->logged = TRUE;
		}
	}

	if (!ohos_avcodec_push(h264, sys, pSrcData, SrcSize))
		return -1;

	if (ohos_avcodec_pull(h264, sys))
	{
		sys->hasFrame = TRUE;
		return 1;
	}

	/* The OHOS decoder emits a frame only when the *next* access unit arrives
	 * (H.264 has no end-of-frame marker), so in steady state this push decoded
	 * the previous AU. If we already have a frame, FreeRDP can composite the
	 * one-frame-old picture. Only the very first AU needs a flush: push it
	 * again to bootstrap. */
	if (sys->hasFrame)
		return 1;

	if (!ohos_avcodec_push(h264, sys, pSrcData, SrcSize))
		return -1;
	if (!ohos_avcodec_pull(h264, sys))
		return -1;
	sys->hasFrame = TRUE;
	return 1;
}

static BOOL ohos_avcodec_init(H264_CONTEXT* h264)
{
	WINPR_ASSERT(h264);
	if (!h264->pSystemData)
	{
		H264_CONTEXT_OHOS_AVCODEC* sys =
		    (H264_CONTEXT_OHOS_AVCODEC*)calloc(1, sizeof(H264_CONTEXT_OHOS_AVCODEC));
		if (!sys)
			return FALSE;
		h264->pSystemData = sys;
	}
	return TRUE;
}

static void ohos_avcodec_uninit(H264_CONTEXT* h264)
{
	if (!h264)
		return;
	H264_CONTEXT_OHOS_AVCODEC* sys = (H264_CONTEXT_OHOS_AVCODEC*)h264->pSystemData;
	if (sys)
	{
		ohos_avcodec_close(sys);
		free(sys);
		h264->pSystemData = NULL;
	}
}

const H264_CONTEXT_SUBSYSTEM g_Subsystem_ohos_avcodec = { "ohos_avcodec", ohos_avcodec_init,
	                                                      ohos_avcodec_uninit,
	                                                      ohos_avcodec_decompress, NULL };
