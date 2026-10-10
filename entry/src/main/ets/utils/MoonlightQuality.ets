import { ML_BITRATE_MAX_KBPS, ML_BITRATE_MIN_KBPS, ML_CODEC_AV1, ML_CODEC_H264, ML_CODEC_HEVC,
  ML_FPS_MAX, ML_FPS_MIN } from '../model/Connections';

// Bits per pixel per frame by codec, calibrated to common recommended bitrates
// (1080p60 H.264 ~ 20 Mbps, 4K60 HEVC ~ 50 Mbps). AUTO assumes a modern codec wins.
const BPP_H264: number = 0.15;
const BPP_HEVC: number = 0.11;
const BPP_AV1: number = 0.08;
const BPP_AUTO: number = 0.11;

export function clampFps(value: number): number {
  if (Number.isNaN(value) || value < ML_FPS_MIN) {
    return ML_FPS_MIN;
  }
  return value > ML_FPS_MAX ? ML_FPS_MAX : value;
}

export function recommendedBitrateKbps(width: number, height: number, fps: number,
  codec: number): number {
  const bpp: number = codec === ML_CODEC_H264 ? BPP_H264
    : codec === ML_CODEC_HEVC ? BPP_HEVC
      : codec === ML_CODEC_AV1 ? BPP_AV1 : BPP_AUTO;
  const rawKbps: number = width * height * fps * bpp / 1000;
  const rounded: number = Math.round(rawKbps / 1000) * 1000;
  return Math.min(Math.max(rounded, ML_BITRATE_MIN_KBPS), ML_BITRATE_MAX_KBPS);
}

export function kbpsToMbps(kbps: number): number {
  return Math.round(kbps / 100) / 10;
}

export function mbpsToKbps(mbps: number): number {
  return Math.round(mbps * 1000);
}
