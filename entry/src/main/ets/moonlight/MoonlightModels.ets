/*
 * HmRdp - Moonlight (GameStream) protocol models.
 *
 * Plain data the ArkTS control layer produces from the host's XML responses; the
 * native side never parses XML (doc_agent/moonlight-native.md §4.2).
 */

/** Fields of one `/serverinfo` response that the client uses. */
export class NvServerInfo {
  hostname: string = '';
  uniqueId: string = '';
  httpsPort: number = 47984;
  externalPort: number = 47989;
  mac: string = '';
  state: string = '';
  gpuType: string = '';
  gfeVersion: string = '';
  // PairStatus == "1" on the host.
  paired: boolean = false;
  // First component of appversion (the "generation"; >= 7 selects SHA-256).
  majorVersion: number = 0;
  // The full appversion text, passed to SERVER_INFORMATION as-is.
  appVersion: string = '';
  // Bitmask of codecs the host can encode (MS-RDPEGFX-style GameStream bits).
  serverCodecModeSupport: number = 0;
  currentGame: number = 0;
}

/** One application the host advertises (`/applist`). */
export class NvApp {
  id: number = 0;
  name: string = '';
  hdr: boolean = false;
}

/** Result of a `/serverinfo` probe. */
export class ServerInfoResult {
  ok: boolean = false;
  // Native HTTP error code or a stable token ("xml", "status").
  error: string = '';
  info: NvServerInfo = new NvServerInfo();
}

/** Result of an `/applist` probe. */
export class AppListResult {
  ok: boolean = false;
  error: string = '';
  apps: NvApp[] = [];
}

/** Result of a pairing attempt. */
export class PairResult {
  ok: boolean = false;
  // "pin" / "mitm" / "already" / native error.
  error: string = '';
}

/** The parameters of one `/launch` request, from the resolved stream profile. */
export class MoonlightLaunchOptions {
  appId: number = 0;
  width: number = 1920;
  height: number = 1080;
  fps: number = 60;
  bitrateKbps: number = 20000;
  audioConfig: number = 0;
  hdr: boolean = false;
  yuv444: boolean = false;
  gamepad: boolean = true;
  // 16 random bytes as hex (the stream's input AES key) and its id; the host
  // echoes nothing, so the caller fills STREAM_CONFIGURATION from these.
  rikeyHex: string = '';
  rikeyId: number = 0;
  localAudio: boolean = false;
  surroundAudio: boolean = false;
}

/** Result of a `/launch` (or `/resume`) request. */
export class LaunchResult {
  ok: boolean = false;
  error: string = '';
  // RTSP session URL for LiStartConnection (`rtspSessionUrl`).
  sessionUrl: string = '';
  rikeyHex: string = '';
  rikeyId: number = 0;
  gameSession: string = '';
}
