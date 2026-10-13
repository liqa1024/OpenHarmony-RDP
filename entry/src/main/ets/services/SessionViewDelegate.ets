import { UIContext } from '@kit.ArkUI';
import { SessionSignal } from '../model/SessionSignal';
import { ConnectionState } from './SessionManager';
import { RemoteCursor } from './RemoteCursor';
import { SessionBackend } from './SessionBackend';
import { SessionInput } from './SessionInput';
import { AppSettings } from './SettingsStore';

export interface SessionShellHost {
  ui: () => UIContext;
  setState: (state: ConnectionState) => void;
  setStatusText: (text: string) => void;
  setOverlayVisible: (visible: boolean) => void;
  setToolbarVisible: (visible: boolean) => void;
  setFullscreen: (fullscreen: boolean) => void;
  setRevealFill: (fill: number) => void;
  isFullscreen: () => boolean;
  isToolbarVisible: () => boolean;
  revealFill: () => number;
  toolbarHeight: () => number;
  hoverDelay: () => number;
  hideDelay: () => number;
  animateReveal: (fill: number, durationMs: number, curve: Curve) => void;
}

export interface SessionShellHooks {
  overlayDelayMs: () => number;
  isPinned: () => boolean;
  makeInput: (backend: SessionBackend | undefined, settings: AppSettings) => SessionInput | undefined;
  configureCursor: (cursor: RemoteCursor, settings: AppSettings) => void;
  onStateChanged: (state: ConnectionState, message: string) => void;
  onSignal: (signal: SessionSignal) => void;
  onSurfaceCreated: (surfaceId: string, width: number, height: number) => void;
  onSurfaceChanged: (width: number, height: number) => void;
  onSurfaceDestroyed: () => void;
  onWindowVisible: (visible: boolean) => void;
  consumesAxis: (event: AxisEvent) => boolean;
  onReleaseInputs: () => void;
  onBeforeRetry: () => void;
  onDisconnectFallback: () => void;
  onRelease: () => void;
}
