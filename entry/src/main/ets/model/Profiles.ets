export const SR_BACKEND_XENGINE: number = 0;
export const SR_BACKEND_FSR: number = 1;

export class DisplayProfile {
  width: number = 0;
  height: number = 0;
  sessionWidth: number = 0;
  sessionHeight: number = 0;
  scalePercent: number = 100;
  srEnabled: boolean = false;
  srRatioPercent: number = 0;
  srBackend: number = SR_BACKEND_XENGINE;
  detected: boolean = false;

  valid(): boolean {
    return this.width > 0 && this.height > 0;
  }
}

export class SrProfile {
  enabled: boolean = false;
  ratioPercent: number = 0;
  backend: number = SR_BACKEND_XENGINE;
}

// A 超分 setting source: a connection's own values, read only when it overrides the protocol's
// globals (SettingsStore.resolveSr). Both protocols' connections provide exactly these four fields.
export interface SrSetting {
  useGlobalSr: boolean;
  srEnabled: boolean;
  srRatioPercent: number;
  srBackend: number;
}

export class AdvancedProfile {
  enableAudio: boolean = true;
  ignoreCertificate: boolean = true;
}
