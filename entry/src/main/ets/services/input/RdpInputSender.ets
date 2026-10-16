import { sendKey, sendLockState, sendMouse, sendTouch, sendUnicode } from 'libhmrdp.so';
import { RdpPointerFlags, RdpTouchFlags } from '../../model/RdpModels';
import { KeyMapping } from '../../utils/KeyMapper';
import { InputAction, InputButton, InputSender, TouchpadContact, TouchPhase } from './InputSink';

// Largest wheel rotation a single RDP pointer event can carry (9-bit wheel field).
const RDP_WHEEL_MAX_CHUNK: number = 0xFF;

// RDP landing of the input verbs: PS/2 scancodes, pointer flags, RDPEI touches, 9-bit wheel chunks.
// No touchpad endpoint and no relative mode. Built from the session handle on the input thread.
export class RdpInputSender implements InputSender {
  private handle: number;
  private lastX: number = 0;
  private lastY: number = 0;

  constructor(handle: number) {
    this.handle = handle;
  }

  setHandle(handle: number): void {
    this.handle = handle;
  }

  key(mapping: KeyMapping, down: boolean): boolean {
    if (this.handle < 0) {
      return true;
    }
    return sendKey(this.handle, mapping.scancode, down, mapping.extended);
  }

  metaKey(down: boolean): boolean {
    if (this.handle < 0) {
      return true;
    }
    return sendKey(this.handle, 0x5B, down, true);
  }

  unicode(code: number, down: boolean): boolean {
    if (this.handle < 0) {
      return true;
    }
    return sendUnicode(this.handle, code, down);
  }

  lockState(flags: number): boolean {
    if (this.handle < 0) {
      return true;
    }
    return sendLockState(this.handle, flags);
  }

  pointerMove(x: number, y: number): boolean {
    this.lastX = x;
    this.lastY = y;
    if (this.handle < 0) {
      return true;
    }
    return sendMouse(this.handle, RdpPointerFlags.MOVE, x, y);
  }

  pointerMoveRelative(_deltaX: number, _deltaY: number): boolean {
    return true;
  }

  button(action: number, button: number, x: number, y: number): boolean {
    this.lastX = x;
    this.lastY = y;
    const flag: number = RdpInputSender.buttonFlag(button);
    if (flag === 0 || this.handle < 0) {
      return true;
    }
    if (action === InputAction.PRESS) {
      sendMouse(this.handle, RdpPointerFlags.MOVE, x, y);
      return sendMouse(this.handle, flag | RdpPointerFlags.DOWN, x, y);
    }
    return sendMouse(this.handle, flag, x, y);
  }

  touch(phase: number, pointerId: number, x: number, y: number, pressure: number): boolean {
    if (this.handle < 0) {
      return true;
    }
    let flags: number = RdpTouchFlags.MOTION;
    if (phase === TouchPhase.DOWN) {
      flags = RdpTouchFlags.DOWN;
    } else if (phase === TouchPhase.UP) {
      flags = RdpTouchFlags.UP;
    }
    const scaled: number = pressure <= 0 ? 0 : Math.max(1, Math.min(1024, Math.round(pressure * 1024)));
    if (scaled > 0) {
      flags |= RdpTouchFlags.HAS_PRESSURE;
    }
    return sendTouch(this.handle, flags, pointerId, scaled, x, y);
  }

  touchpadFrame(_contacts: TouchpadContact[], _buttonState: number, _widthMm: number,
    _heightMm: number): boolean {
    return true;
  }

  scroll(vertical: number, horizontal: number, x: number, y: number): boolean {
    this.lastX = x;
    this.lastY = y;
    const accepted: boolean = this.sendWheel(vertical, false, x, y);
    return this.sendWheel(horizontal, true, x, y) && accepted;
  }

  private sendWheel(amount: number, horizontal: boolean, x: number, y: number): boolean {
    if (this.handle < 0) {
      return true;
    }
    let accepted: boolean = true;
    let remaining: number = amount;
    while (Math.abs(remaining) >= 1) {
      const magnitude: number = Math.min(Math.floor(Math.abs(remaining)), RDP_WHEEL_MAX_CHUNK);
      if (magnitude < 1) {
        break;
      }
      const negative: boolean = remaining < 0;
      // Negative rotation is 9-bit two's complement: magnitude m -> WHEEL_NEGATIVE | (0x100 - m).
      let flags: number = horizontal ? RdpPointerFlags.HWHEEL : RdpPointerFlags.WHEEL;
      flags |= negative
        ? RdpPointerFlags.WHEEL_NEGATIVE | (0x100 - magnitude)
        : magnitude;
      accepted = sendMouse(this.handle, flags, x, y) && accepted;
      remaining -= negative ? -magnitude : magnitude;
    }
    return accepted;
  }

  private static buttonFlag(button: number): number {
    if (button === InputButton.LEFT) {
      return RdpPointerFlags.BUTTON1;
    }
    if (button === InputButton.RIGHT) {
      return RdpPointerFlags.BUTTON2;
    }
    if (button === InputButton.MIDDLE) {
      return RdpPointerFlags.BUTTON3;
    }
    return 0;
  }
}
