import { inputDevice } from '@kit.InputKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import {
  GamepadKitDevice, GamepadKitEventExtra, gamepadKitDevices, gamepadKitSetListener, gamepadKitStop
} from 'libhmrdp.so';
import { GamepadSink } from './InputSink';

const DOMAIN: number = 0xD004;
const TAG: string = 'HmRdpGamepad';

// Game Controller Kit event kinds (GamepadKitEvent) and axis sources (GamePad_AxisSourceType): the
// axis source names the control the event came from, so the two stick pairs and the two triggers are
// never confused for one another.
const KIT_EVENT_DEVICE_ONLINE: number = 0;
const KIT_EVENT_DEVICE_OFFLINE: number = 1;
const KIT_EVENT_AXIS: number = 2;
const KIT_AXIS_DPAD: number = 0;
const KIT_AXIS_LEFT_STICK: number = 1;
const KIT_AXIS_RIGHT_STICK: number = 2;
const KIT_AXIS_LEFT_TRIGGER: number = 3;
const KIT_AXIS_RIGHT_TRIGGER: number = 4;

// Gamepad key codes. The platform maps evdev codes itself (MMI's TransferKeyValue), and an evdev code
// that map has no entry for arrives as 10000 + code: 167 = KEY_RECORD (the share button) and
// 708..711 = BTN_TRIGGER_HAPPY5..8 (the back paddles). Neither reaches the host as itself — the
// protocol has no flag for them and the host folds its misc bit into Guide — so such codes are
// consumed and dropped. Buttons come from these key events in every case: the Game Controller Kit's
// own button set is narrower.
const KEYCODE_BACK: number = 2;
const KEYCODE_DPAD_UP: number = 2012;
const KEYCODE_DPAD_DOWN: number = 2013;
const KEYCODE_DPAD_LEFT: number = 2014;
const KEYCODE_DPAD_RIGHT: number = 2015;
const KEYCODE_DPAD_CENTER: number = 2016;
const KEYCODE_MENU: number = 2067;
const KEYCODE_BUTTON_A: number = 2301;
const KEYCODE_BUTTON_B: number = 2302;
const KEYCODE_BUTTON_X: number = 2304;
const KEYCODE_BUTTON_Y: number = 2305;
const KEYCODE_BUTTON_L1: number = 2307;
const KEYCODE_BUTTON_R1: number = 2308;
const KEYCODE_BUTTON_L2: number = 2309;
const KEYCODE_BUTTON_R2: number = 2310;
const KEYCODE_BUTTON_SELECT: number = 2311;
const KEYCODE_BUTTON_START: number = 2312;
const KEYCODE_BUTTON_MODE: number = 2313;
const KEYCODE_BUTTON_THUMBL: number = 2314;
const KEYCODE_BUTTON_THUMBR: number = 2315;
const KEYCODE_BUTTON_FIRST: number = 2301;
const KEYCODE_BUTTON_LAST: number = 2315;
const KEYCODE_BUTTON_AUX_FIRST: number = 2401;
const KEYCODE_BUTTON_AUX_LAST: number = 2416;
const KEYCODE_BUTTON_GENERIC_FIRST: number = 3100;
const KEYCODE_BUTTON_GENERIC_LAST: number = 3109;
const KEYCODE_UNMAPPED_BASE: number = 10000;
const KEYCODE_KEY_RECORD: number = KEYCODE_UNMAPPED_BASE + 167;
const KEYCODE_BTN_MISC: number = 2662;
const KEYCODE_BTN_DPAD_FIRST: number = KEYCODE_UNMAPPED_BASE + 544;
const KEYCODE_BTN_DPAD_LAST: number = KEYCODE_UNMAPPED_BASE + 547;
const KEYCODE_PADDLE_FIRST: number = KEYCODE_UNMAPPED_BASE + 708;
const KEYCODE_PADDLE_LAST: number = KEYCODE_UNMAPPED_BASE + 711;

// Controller slots, i.e. the width of activeGamepadMask the host is asked to honour (GFE's cap).
const MAX_PADS: number = 4;
// The hat axes are reported as raw -1/0/1, so half-scale separates a direction from the centre.
const HAT_THRESHOLD: number = 0.5;
const STICK_LIMIT: number = 32767;
const TRIGGER_LIMIT: number = 255;
// A trigger axis that idles at the negative end reads (value + 1) / 2, one that idles at 0 reads
// as-is; the two are told apart by the lowest value the axis has shown.
const TRIGGER_IDLE_NEGATIVE: number = -0.25;
// A release is held back to this down time: a tap arrives as a down/up pair a few ms apart, and a
// host that samples the pad at its own rate would otherwise miss the press entirely.
const MIN_BUTTON_DOWN_MS: number = 25;

// LiSendMultiControllerEvent / LiSendControllerArrivalEvent wire constants.
export class MlController {
  static readonly FLAG_UP: number = 0x0001;
  static readonly FLAG_DOWN: number = 0x0002;
  static readonly FLAG_LEFT: number = 0x0004;
  static readonly FLAG_RIGHT: number = 0x0008;
  static readonly FLAG_PLAY: number = 0x0010;
  static readonly FLAG_BACK: number = 0x0020;
  static readonly FLAG_LS_CLK: number = 0x0040;
  static readonly FLAG_RS_CLK: number = 0x0080;
  static readonly FLAG_LB: number = 0x0100;
  static readonly FLAG_RB: number = 0x0200;
  static readonly FLAG_SPECIAL: number = 0x0400;
  static readonly FLAG_A: number = 0x1000;
  static readonly FLAG_B: number = 0x2000;
  static readonly FLAG_X: number = 0x4000;
  static readonly FLAG_Y: number = 0x8000;
  // The advertised key set is the Xbox layout (the KeyCode names are A/B/X/Y) and the sticks and
  // triggers are analog; nothing here can rumble (yet) or report touchpad / motion / battery state.
  // The paddles (PADDLE1..4) and MISC have flags in the protocol, but no host consumes the paddles
  // and the host folds MISC into Guide — so neither is advertised or sent.
  static readonly TYPE_XBOX: number = 0x01;
  static readonly CAP_ANALOG_TRIGGERS: number = 0x01;
  static readonly SUPPORTED_BUTTONS: number =
    MlController.FLAG_A | MlController.FLAG_B | MlController.FLAG_X | MlController.FLAG_Y
      | MlController.FLAG_UP | MlController.FLAG_DOWN | MlController.FLAG_LEFT
      | MlController.FLAG_RIGHT | MlController.FLAG_LB | MlController.FLAG_RB
      | MlController.FLAG_PLAY | MlController.FLAG_BACK | MlController.FLAG_LS_CLK
      | MlController.FLAG_RS_CLK | MlController.FLAG_SPECIAL;
}

// One attached controller: its slot state, with the axes kept per role (the role is what the Game
// Controller Kit reports, or the documented layout when it is unavailable).
class GamepadPad {
  deviceId: number = 0;
  arrived: boolean = false;
  buttons: number = 0;
  // Digital L2/R2 fallbacks, for pads whose triggers are plain buttons; -1 = no digital report.
  digitalLeft: number = -1;
  digitalRight: number = -1;
  leftX: number = 0;
  leftY: number = 0;
  rightX: number = 0;
  rightY: number = 0;
  triggerLeft: number = 0;
  triggerRight: number = 0;
  // Lowest raw trigger value seen, which tells an idle-negative trigger axis from a unipolar one.
  triggerMin: number = 0;
  hatX: number = 0;
  hatY: number = 0;
  // Once a hat axis has actually moved it owns the direction bits, and the equivalent key events are
  // ignored (pads report the same D-pad press both as an axis and as keys).
  hatXUsed: boolean = false;
  hatYUsed: boolean = false;
  // Button-flag down time and the releases held back to MIN_BUTTON_DOWN_MS.
  downAt: Map<number, number> = new Map<number, number>();
  pendingUp: Map<number, number> = new Map<number, number>();
  upTimer: number = -1;
}

// Gamepad policy for Moonlight. Axes: the platform's Game Controller Kit is the source of truth
// (each axis event names the control it came from); when it is unavailable the multimodal input
// focus axis events are read by the documented layout (X/Y = left stick, Z/RZ = right stick,
// GAS/BRAKE = the triggers, HAT = the D-pad). Buttons: always the multimodal input key events,
// which cover more than the Kit's own button set (share / capture, paddles, Guide).
// RDP has no gamepad path, so this is not part of the shared InputSink.
export class GamepadInput {
  private sink: GamepadSink;
  private pads: Map<number, GamepadPad> = new Map<number, GamepadPad>();
  private kitActive: boolean = false;
  private kitDeviceIds: Map<string, number> = new Map<string, number>();
  private deviceChangeListener: (data: inputDevice.DeviceListener) => void =
    (data: inputDevice.DeviceListener): void => {
      GamepadDevices.refresh();
      if (data.type === 'add') {
        this.onDeviceAdded(data.deviceId);
      } else {
        this.onDeviceRemoved(data.deviceId);
      }
    };
  private kitListener: (event: number, source: number, extra: GamepadKitEventExtra) => void =
    (event: number, source: number, extra: GamepadKitEventExtra): void => {
      this.onKitEvent(event, source, extra);
    };

  constructor(sink: GamepadSink) {
    this.sink = sink;
    GamepadDevices.refresh();
    try {
      inputDevice.on('change', this.deviceChangeListener);
    } catch (error) {
    }
    // The Kit labels the axes; without it the documented layout is used instead.
    this.kitActive = gamepadKitSetListener(this.kitListener);
    hilog.info(DOMAIN, TAG, 'gamepad axes from %{public}s',
      this.kitActive ? 'Game Controller Kit' : 'multimodal input events');
  }

  dispose(): void {
    if (this.kitActive) {
      gamepadKitStop();
      this.kitActive = false;
    }
    try {
      inputDevice.off('change', this.deviceChangeListener);
    } catch (error) {
    }
    for (let slot = 0; slot < MAX_PADS; slot++) {
      const pad: GamepadPad | undefined = this.pads.get(slot);
      if (pad !== undefined) {
        this.clearTimers(pad);
      }
    }
    this.pads.clear();
  }

  // Returns true for every gamepad key, handled or not: those codes must not fall through to the
  // keyboard path (the D-pad would otherwise reach the remote as arrow keys).
  handleKey(event: KeyEvent): boolean {
    const keyCode: number = event.keyCode;
    if (!GamepadInput.isGamepadKey(event)) {
      return false;
    }
    const slot: number = this.slotFor(event.deviceId);
    const pad: GamepadPad | undefined = slot >= 0 ? this.pads.get(slot) : undefined;
    if (pad === undefined) {
      return true;
    }
    const down: boolean = event.type === KeyType.Down;
    if (GamepadInput.isDuplicateHat(pad, keyCode)) {
      // The hat axis drives this direction; the matching key event is the same press reported twice.
      return true;
    }
    const flag: number = GamepadInput.flagFor(keyCode);
    if (flag !== 0) {
      this.press(slot, pad, flag, down);
      this.publish(slot, pad);
      return true;
    }
    if (keyCode === KEYCODE_BUTTON_L2 || keyCode === KEYCODE_BUTTON_R2) {
      // A pad with a real trigger axis overrides this when the state is sent; a button-only pad keeps
      // this full-press value.
      if (keyCode === KEYCODE_BUTTON_L2) {
        pad.digitalLeft = down ? TRIGGER_LIMIT : 0;
      } else {
        pad.digitalRight = down ? TRIGGER_LIMIT : 0;
      }
      this.publish(slot, pad);
      return true;
    }
    // A button whose code the platform has no flag for and whose evdev meaning we do not know: it is
    // consumed and dropped. Guessing a protocol bucket for it would put a wrong button on the host.
    return true;
  }

  handleFocusAxis(event: FocusAxisEvent): void {
    if (this.kitActive) {
      // The Kit is feeding the axes, with the control named; the raw event would only duplicate it.
      return;
    }
    const slot: number = this.slotFor(event.deviceId ?? 0);
    const pad: GamepadPad | undefined = slot >= 0 ? this.pads.get(slot) : undefined;
    if (pad === undefined) {
      return;
    }
    const map: Map<AxisModel, number> = event.axisMap;
    const leftX: number | undefined = map.get(AxisModel.ABS_X);
    const leftY: number | undefined = map.get(AxisModel.ABS_Y);
    const rightX: number | undefined = map.get(AxisModel.ABS_Z);
    const rightY: number | undefined = map.get(AxisModel.ABS_RZ);
    const leftTrigger: number | undefined = map.get(AxisModel.ABS_BRAKE);
    const rightTrigger: number | undefined = map.get(AxisModel.ABS_GAS);
    const hatX: number | undefined = map.get(AxisModel.ABS_HAT0X);
    const hatY: number | undefined = map.get(AxisModel.ABS_HAT0Y);
    if (leftX !== undefined) {
      pad.leftX = leftX;
    }
    if (leftY !== undefined) {
      pad.leftY = leftY;
    }
    if (rightX !== undefined) {
      pad.rightX = rightX;
    }
    if (rightY !== undefined) {
      pad.rightY = rightY;
    }
    if (leftTrigger !== undefined) {
      pad.triggerLeft = GamepadInput.triggerUnit(pad, leftTrigger);
    }
    if (rightTrigger !== undefined) {
      pad.triggerRight = GamepadInput.triggerUnit(pad, rightTrigger);
    }
    if (hatX !== undefined) {
      pad.hatX = hatX;
      if (Math.abs(hatX) >= HAT_THRESHOLD) {
        pad.hatXUsed = true;
      }
    }
    if (hatY !== undefined) {
      pad.hatY = hatY;
      if (Math.abs(hatY) >= HAT_THRESHOLD) {
        pad.hatYUsed = true;
      }
    }
    this.publish(slot, pad);
  }

  // Focus left the picture: every held button must be lifted, or the remote keeps it down.
  releaseAll(): void {
    const mask: number = this.mask();
    for (let slot = 0; slot < MAX_PADS; slot++) {
      const pad: GamepadPad | undefined = this.pads.get(slot);
      if (pad === undefined) {
        continue;
      }
      GamepadInput.resetState(pad);
      this.clearTimers(pad);
      this.sink.controllerState(slot, mask, 0, 0, 0, 0, 0, 0, 0);
    }
  }

  private onKitEvent(event: number, source: number, extra: GamepadKitEventExtra): void {
    const key: string = extra.deviceKey ?? '';
    const name: string = extra.name ?? '';
    if (event === KIT_EVENT_DEVICE_ONLINE) {
      // Announce the controller now (its arrival declaration goes out before the first input).
      this.kitDeviceIds.delete(key);
      GamepadDevices.refresh();
      const deviceId: number = this.resolveKitDevice(key, name);
      if (deviceId >= 0) {
        const slot: number = this.slotFor(deviceId);
        const pad: GamepadPad | undefined = slot >= 0 ? this.pads.get(slot) : undefined;
        if (pad !== undefined && !pad.arrived) {
          this.publish(slot, pad);
        }
      }
      return;
    }
    if (event === KIT_EVENT_DEVICE_OFFLINE) {
      // Slot bookkeeping follows the multimodal input device events; this only drops the tie.
      this.kitDeviceIds.delete(key);
      return;
    }
    if (event !== KIT_EVENT_AXIS) {
      return;
    }
    const slot: number = this.slotForKit(key, name);
    const pad: GamepadPad | undefined = slot >= 0 ? this.pads.get(slot) : undefined;
    if (pad === undefined) {
      return;
    }
    const first: number = extra.value1 ?? 0;
    const second: number = extra.value2 ?? 0;
    if (source === KIT_AXIS_LEFT_STICK) {
      pad.leftX = first;
      pad.leftY = second;
    } else if (source === KIT_AXIS_RIGHT_STICK) {
      pad.rightX = first;
      pad.rightY = second;
    } else if (source === KIT_AXIS_LEFT_TRIGGER) {
      pad.triggerLeft = GamepadInput.triggerUnit(pad, first);
    } else if (source === KIT_AXIS_RIGHT_TRIGGER) {
      pad.triggerRight = GamepadInput.triggerUnit(pad, first);
    } else if (source === KIT_AXIS_DPAD) {
      pad.hatX = first;
      pad.hatY = second;
      if (Math.abs(first) >= HAT_THRESHOLD) {
        pad.hatXUsed = true;
      }
      if (Math.abs(second) >= HAT_THRESHOLD) {
        pad.hatYUsed = true;
      }
    } else {
      return;
    }
    this.publish(slot, pad);
  }

  // A Kit axis event has to land on the same slot as that controller's key events. When the Kit's
  // device key cannot be tied to a key-event device id, a single attached pad is still unambiguous;
  // with several in play the event is dropped rather than inventing a second slot for one pad.
  private slotForKit(key: string, name: string): number {
    const deviceId: number = this.resolveKitDevice(key, name);
    if (deviceId >= 0) {
      return this.slotFor(deviceId);
    }
    if (this.pads.size === 1) {
      for (let slot = 0; slot < MAX_PADS; slot++) {
        if (this.pads.has(slot)) {
          return slot;
        }
      }
    }
    return -1;
  }

  // The Kit names a controller by its own key, while the key events carry the multimodal input
  // device id; the two are tied together here (see GamepadDevices.resolveDeviceId).
  private resolveKitDevice(key: string, name: string): number {
    const cached: number | undefined = this.kitDeviceIds.get(key);
    if (cached !== undefined) {
      return cached;
    }
    const deviceId: number = GamepadDevices.resolveDeviceId(key, name);
    if (deviceId >= 0) {
      this.kitDeviceIds.set(key, deviceId);
    }
    return deviceId;
  }

  // A tap arrives as a down/up pair a few ms apart, while the host samples the pad at its own rate
  // (and games sample that again per frame), so a very short press would be lost. The release is
  // therefore held back to a minimum down time — the same guard the Android client applies.
  private press(slot: number, pad: GamepadPad, flag: number, down: boolean): void {
    const now: number = Date.now();
    if (down) {
      pad.downAt.set(flag, now);
      pad.pendingUp.delete(flag);
      pad.buttons |= flag;
      return;
    }
    const at: number | undefined = pad.downAt.get(flag);
    const remaining: number = at === undefined ? 0 : MIN_BUTTON_DOWN_MS - (now - at);
    if (remaining <= 0) {
      pad.buttons &= ~flag;
      return;
    }
    pad.pendingUp.set(flag, now + remaining);
    this.scheduleRelease(slot, pad, remaining);
  }

  private scheduleRelease(slot: number, pad: GamepadPad, delayMs: number): void {
    if (pad.upTimer !== -1) {
      return;
    }
    pad.upTimer = setTimeout((): void => {
      pad.upTimer = -1;
      const now: number = Date.now();
      const due: number[] = [];
      let next: number = -1;
      pad.pendingUp.forEach((at: number, flag: number): void => {
        if (at <= now) {
          due.push(flag);
        } else if (next < 0 || at < next) {
          next = at;
        }
      });
      for (let i = 0; i < due.length; i++) {
        pad.buttons &= ~due[i];
        pad.pendingUp.delete(due[i]);
      }
      if (due.length > 0) {
        this.publish(slot, pad);
      }
      if (next >= 0) {
        this.scheduleRelease(slot, pad, next - Date.now());
      }
    }, delayMs > 0 ? delayMs : 1);
  }

  private clearTimers(pad: GamepadPad): void {
    if (pad.upTimer !== -1) {
      clearTimeout(pad.upTimer);
      pad.upTimer = -1;
    }
    pad.pendingUp.clear();
  }

  private publish(slot: number, pad: GamepadPad): void {
    if (!pad.arrived) {
      // Declares the pad's type and capabilities so the host emulates a fitting controller; the
      // library falls back to a plain state event when the host does not support it.
      this.sink.controllerArrival(slot, this.mask(), MlController.TYPE_XBOX,
        MlController.SUPPORTED_BUTTONS, MlController.CAP_ANALOG_TRIGGERS);
      pad.arrived = true;
    }
    this.sink.controllerState(slot, this.mask(), GamepadInput.buttons(pad),
      GamepadInput.triggerValue(pad.triggerLeft, pad.digitalLeft),
      GamepadInput.triggerValue(pad.triggerRight, pad.digitalRight),
      GamepadInput.stick(pad.leftX), GamepadInput.stick(-pad.leftY),
      GamepadInput.stick(pad.rightX), GamepadInput.stick(-pad.rightY));
  }

  private onDeviceAdded(deviceId: number): void {
    if (!GamepadDevices.isJoystick(deviceId)) {
      return;
    }
    const slot: number = this.slotFor(deviceId);
    const pad: GamepadPad | undefined = slot >= 0 ? this.pads.get(slot) : undefined;
    if (pad !== undefined && !pad.arrived) {
      this.publish(slot, pad);
    }
  }

  private onDeviceRemoved(deviceId: number): void {
    const slot: number = this.slotOf(deviceId);
    if (slot < 0) {
      return;
    }
    const pad: GamepadPad | undefined = this.pads.get(slot);
    if (pad !== undefined) {
      this.clearTimers(pad);
    }
    this.pads.delete(slot);
    // A zeroed state with the bit dropped is how the host frees the virtual controller.
    this.sink.controllerState(slot, this.mask(), 0, 0, 0, 0, 0, 0, 0);
  }

  private static resetState(pad: GamepadPad): void {
    pad.buttons = 0;
    pad.digitalLeft = -1;
    pad.digitalRight = -1;
    pad.leftX = 0;
    pad.leftY = 0;
    pad.rightX = 0;
    pad.rightY = 0;
    pad.triggerLeft = 0;
    pad.triggerRight = 0;
    pad.hatX = 0;
    pad.hatY = 0;
    pad.downAt.clear();
  }

  // A code a gamepad can produce; the platform reports its buttons as joystick-sourced key events,
  // with the gamepad-only code ranges as a fallback for pads whose source is not flagged as one.
  // Codes a keyboard also has (back / menu / D-pad centre) are only taken from a joystick source.
  private static isGamepadKey(event: KeyEvent): boolean {
    if (event.keySource === KeySource.JOYSTICK) {
      return true;
    }
    const keyCode: number = event.keyCode;
    if (keyCode >= KEYCODE_BUTTON_FIRST && keyCode <= KEYCODE_BUTTON_LAST) {
      return true;
    }
    if (keyCode >= KEYCODE_BUTTON_AUX_FIRST && keyCode <= KEYCODE_BUTTON_AUX_LAST) {
      return true;
    }
    if (keyCode >= KEYCODE_BUTTON_GENERIC_FIRST && keyCode <= KEYCODE_BUTTON_GENERIC_LAST) {
      return true;
    }
    if (keyCode >= KEYCODE_PADDLE_FIRST && keyCode <= KEYCODE_PADDLE_LAST) {
      return true;
    }
    if (keyCode >= KEYCODE_BTN_DPAD_FIRST && keyCode <= KEYCODE_BTN_DPAD_LAST) {
      return true;
    }
    return keyCode === KEYCODE_BTN_MISC || keyCode === KEYCODE_KEY_RECORD;
  }

  // Buttons by evdev meaning: the platform's key map is a fixed evdev -> KeyCode table, so a code
  // means the same thing on every pad. The D-pad's centre press is the A button on the pads that
  // report it. Nothing is sent for a code the protocol has no flag for (see the comment on the key
  // codes): the host would put a wrong button on the virtual pad.
  private static flagFor(keyCode: number): number {
    switch (keyCode) {
      case KEYCODE_BUTTON_A: return MlController.FLAG_A;
      case KEYCODE_DPAD_CENTER: return MlController.FLAG_A;
      case KEYCODE_BUTTON_B: return MlController.FLAG_B;
      case KEYCODE_BUTTON_X: return MlController.FLAG_X;
      case KEYCODE_BUTTON_Y: return MlController.FLAG_Y;
      case KEYCODE_BUTTON_L1: return MlController.FLAG_LB;
      case KEYCODE_BUTTON_R1: return MlController.FLAG_RB;
      case KEYCODE_BUTTON_SELECT: return MlController.FLAG_BACK;
      case KEYCODE_BACK: return MlController.FLAG_BACK;
      case KEYCODE_BUTTON_START: return MlController.FLAG_PLAY;
      // Some pads report their start button as the menu key.
      case KEYCODE_MENU: return MlController.FLAG_PLAY;
      case KEYCODE_BUTTON_MODE: return MlController.FLAG_SPECIAL;
      case KEYCODE_BUTTON_THUMBL: return MlController.FLAG_LS_CLK;
      case KEYCODE_BUTTON_THUMBR: return MlController.FLAG_RS_CLK;
      case KEYCODE_DPAD_UP: return MlController.FLAG_UP;
      case KEYCODE_DPAD_DOWN: return MlController.FLAG_DOWN;
      case KEYCODE_DPAD_LEFT: return MlController.FLAG_LEFT;
      case KEYCODE_DPAD_RIGHT: return MlController.FLAG_RIGHT;
      case KEYCODE_BTN_DPAD_FIRST: return MlController.FLAG_UP;
      case KEYCODE_BTN_DPAD_FIRST + 1: return MlController.FLAG_DOWN;
      case KEYCODE_BTN_DPAD_FIRST + 2: return MlController.FLAG_LEFT;
      case KEYCODE_BTN_DPAD_FIRST + 3: return MlController.FLAG_RIGHT;
      default: return 0;
    }
  }

  private static isDuplicateHat(pad: GamepadPad, keyCode: number): boolean {
    if (keyCode === KEYCODE_DPAD_LEFT || keyCode === KEYCODE_DPAD_RIGHT) {
      return pad.hatXUsed;
    }
    if (keyCode === KEYCODE_DPAD_UP || keyCode === KEYCODE_DPAD_DOWN) {
      return pad.hatYUsed;
    }
    return false;
  }

  // The buttons the attached pad reports as keys, plus the hat directions once that pad has actually
  // turned out to drive them (before that the key-based directions stand on their own).
  private static buttons(pad: GamepadPad): number {
    let buttons: number = pad.buttons;
    if (pad.hatXUsed) {
      buttons &= ~(MlController.FLAG_LEFT | MlController.FLAG_RIGHT);
      if (pad.hatX <= -HAT_THRESHOLD) {
        buttons |= MlController.FLAG_LEFT;
      } else if (pad.hatX >= HAT_THRESHOLD) {
        buttons |= MlController.FLAG_RIGHT;
      }
    }
    if (pad.hatYUsed) {
      buttons &= ~(MlController.FLAG_UP | MlController.FLAG_DOWN);
      if (pad.hatY <= -HAT_THRESHOLD) {
        buttons |= MlController.FLAG_UP;
      } else if (pad.hatY >= HAT_THRESHOLD) {
        buttons |= MlController.FLAG_DOWN;
      }
    }
    return buttons;
  }

  // Sticks arrive normalized to [-1, 1]; the wire wants [-32767, 32767] with +Y up, so the caller
  // negates Y (the convention the reference clients use against the same hosts).
  private static stick(value: number): number {
    return Math.max(-STICK_LIMIT, Math.min(STICK_LIMIT, Math.round(value * STICK_LIMIT)));
  }

  // A trigger axis idles either at 0 (unipolar) or at the negative end: both are folded into 0..1
  // here, using the lowest value the axis has shown as the tell.
  private static triggerUnit(pad: GamepadPad, raw: number): number {
    if (raw < pad.triggerMin) {
      pad.triggerMin = raw;
    }
    const unit: number = raw <= TRIGGER_IDLE_NEGATIVE || pad.triggerMin <= TRIGGER_IDLE_NEGATIVE
      ? (raw + 1) / 2 : raw;
    return Math.max(0, Math.min(1, unit));
  }

  private static triggerValue(unit: number, digital: number): number {
    const value: number = Math.max(unit * TRIGGER_LIMIT, digital);
    return Math.max(0, Math.min(TRIGGER_LIMIT, Math.round(value)));
  }

  private slotOf(deviceId: number): number {
    for (let slot = 0; slot < MAX_PADS; slot++) {
      const pad: GamepadPad | undefined = this.pads.get(slot);
      if (pad !== undefined && pad.deviceId === deviceId) {
        return slot;
      }
    }
    return -1;
  }

  private slotFor(deviceId: number): number {
    const existing: number = this.slotOf(deviceId);
    if (existing >= 0) {
      return existing;
    }
    if (deviceId <= 0 && this.pads.size === 1) {
      // An event without a device id: with a single pad attached it can only be that one.
      for (let slot = 0; slot < MAX_PADS; slot++) {
        if (this.pads.has(slot)) {
          return slot;
        }
      }
    }
    for (let slot = 0; slot < MAX_PADS; slot++) {
      if (!this.pads.has(slot)) {
        const pad: GamepadPad = new GamepadPad();
        pad.deviceId = deviceId;
        this.pads.set(slot, pad);
        hilog.info(DOMAIN, TAG, 'pad attached: device %{public}d is controller %{public}d',
          deviceId, slot);
        return slot;
      }
    }
    return -1;
  }

  // Bits of the slots in use; the host creates and frees its virtual controllers from it.
  private mask(): number {
    let mask: number = 0;
    for (let slot = 0; slot < MAX_PADS; slot++) {
      if (this.pads.has(slot)) {
        mask |= (1 << slot);
      }
    }
    return mask;
  }
}

// Connected controllers.
export class GamepadDevices {
  private static ids: number[] = [];

  // One bit per attached controller, in attachment order; 0 when none is attached. The host takes it
  // as the set of controllers to create up front (hot-plug updates it at run time).
  static async attachedMask(): Promise<number> {
    const kit: GamepadKitDevice[] = gamepadKitDevices();
    if (kit.length > 0) {
      return GamepadDevices.maskFor(Math.min(kit.length, MAX_PADS));
    }
    let count: number = 0;
    try {
      const ids: number[] = await inputDevice.getDeviceList();
      for (let i = 0; i < ids.length && count < MAX_PADS; i++) {
        if (GamepadDevices.isJoystick(ids[i])) {
          count++;
        }
      }
    } catch (error) {
      return 0;
    }
    return GamepadDevices.maskFor(count);
  }

  // Cache of the multimodal input ids of the joystick-class devices; refreshed on hot-plug.
  static refresh(): void {
    inputDevice.getDeviceList((error: BusinessError, ids: number[]): void => {
      if (error !== undefined && error !== null) {
        return;
      }
      const joysticks: number[] = [];
      for (let i = 0; i < ids.length; i++) {
        if (GamepadDevices.isJoystick(ids[i])) {
          joysticks.push(ids[i]);
        }
      }
      GamepadDevices.ids = joysticks;
    });
  }

  // Ties the Kit's own device key to the multimodal input device the key events come from: the key
  // itself when it is the id in text form, otherwise the device name, otherwise the single
  // candidate. -1 when it cannot be settled.
  static resolveDeviceId(key: string, name: string): number {
    const ids: number[] = GamepadDevices.ids;
    const numeric: number = Number.parseInt(key, 10);
    if (!Number.isNaN(numeric) && GamepadDevices.isJoystick(numeric)) {
      return numeric;
    }
    if (name.length > 0) {
      for (let i = 0; i < ids.length; i++) {
        if (GamepadDevices.deviceName(ids[i]) === name) {
          return ids[i];
        }
      }
    }
    return ids.length === 1 ? ids[0] : -1;
  }

  static isJoystick(deviceId: number): boolean {
    if (deviceId <= 0) {
      return false;
    }
    try {
      const info: inputDevice.InputDeviceData = inputDevice.getDeviceInfoSync(deviceId);
      return info.sources.indexOf('joystick') >= 0;
    } catch (error) {
    }
    return false;
  }

  private static deviceName(deviceId: number): string {
    try {
      return inputDevice.getDeviceInfoSync(deviceId).name;
    } catch (error) {
    }
    return '';
  }

  private static maskFor(count: number): number {
    let mask: number = 0;
    for (let i = 0; i < count; i++) {
      mask |= (1 << i);
    }
    return mask;
  }
}
