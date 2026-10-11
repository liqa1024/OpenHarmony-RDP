export class NvServerInfo {
  hostname: string = '';
  uniqueId: string = '';
  httpsPort: number = 47984;
  externalPort: number = 47989;
  mac: string = '';
  state: string = '';
  gpuType: string = '';
  gfeVersion: string = '';
  paired: boolean = false;
  // First component of appversion; >= 7 selects SHA-256.
  majorVersion: number = 0;
  appVersion: string = '';
  serverCodecModeSupport: number = 0;
  currentGame: number = 0;
}

export class NvApp {
  id: number = 0;
  name: string = '';
}

export class ServerInfoResult {
  ok: boolean = false;
  error: string = '';
  info: NvServerInfo = new NvServerInfo();
}

export class AppListResult {
  ok: boolean = false;
  error: string = '';
  apps: NvApp[] = [];
}

export class PairResult {
  ok: boolean = false;
  error: string = '';
}

export class MoonlightLaunchOptions {
  appId: number = 0;
  width: number = 1920;
  height: number = 1080;
  fps: number = 60;
  bitrateKbps: number = 20000;
  rikeyHex: string = '';
  rikeyId: number = 0;
  // Client audio on: host plays locally off. Drives the /launch localAudioPlayMode.
  audioEnabled: boolean = true;
}

export class LaunchResult {
  ok: boolean = false;
  error: string = '';
  sessionUrl: string = '';
  rikeyHex: string = '';
  rikeyId: number = 0;
}
