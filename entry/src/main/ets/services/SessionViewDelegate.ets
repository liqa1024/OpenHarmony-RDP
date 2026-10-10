import { UIContext } from '@kit.ArkUI';
import { SessionSignal } from '../model/SessionSignal';
import { ConnectionState } from './SessionManager';
import { RemoteCursor } from './RemoteCursor';
import { SessionBackend } from './SessionBackend';
import { SessionInput } from './SessionInput';
import { AppSettings } from './SettingsStore';

// Shared, protocol-neutral state setters the shell controller uses to drive the
// page's @State. Implemented as closures by each session page. Members are
// function-typed properties so the page can supply a typed object literal.
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

// Protocol-specific policy the shell controller calls back into. Each session page
// supplies these closures; the controller itself never names a protocol.
export interface SessionShellHooks {
  // 0 shows the connecting card at once; > 0 holds it back (avoids a flash on a fast start).
  overlayDelayMs: () => number;
  // A running transfer may pin the toolbar open; protocol-specific.
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
