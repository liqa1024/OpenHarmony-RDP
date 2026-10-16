import { KeyMapping } from '../../utils/KeyMapper';
import { MlVirtualKey } from '../../utils/MoonlightKeyMapper';
import { GamepadSink, InputAction, InputButton, InputSender, MoonlightSendPort, TextRunSink,
  TouchpadContact, TouchPhase } from './InputSink';

// Windows VK for the Meta/Win key.
const ML_VK_META: number = 0x5B;

// UNSUPPORTED = the host has no such feature: a by-design refusal, not a drop (plan §5).
const ML_SEND_OK: number = 0;
const ML_SEND_UNSUPPORTED: number = -5501;

function mlAccepted(code: number): boolean {
  return code === ML_SEND_OK || code === ML_SEND_UNSUPPORTED;
}

// Moonlight landing of the input verbs, on the input thread over the library's process-global
// session: a send here just enters the library's queue (its back-pressure stays on its own thread).
export class MoonlightInputSender implements InputSender, GamepadSink, TextRunSink {
  private port: MoonlightSendPort;
  // Left/right modifiers share a bit but are distinct VKs, so the byte is derived
  // from the set of keys currently down, not from a per-key toggle.
  private heldModifiers: Set<number> = new Set<number>();
  private referenceWidth: number = 1920;
  private referenceHeight: number = 1080;
  private relative: boolean = false;

  constructor(port: MoonlightSendPort) {
    this.port = port;
  }

  setReferenceSize(width: number, height: number): void {
    this.referenceWidth = width;
    this.referenceHeight = height;
  }

  setRelativePointer(enabled: boolean): void {
    this.relative = enabled;
  }

  key(mapping: KeyMapping, down: boolean): boolean {
    if (mapping.vk <= 0) {
      return true;
    }
    this.trackModifier(mapping.vk, down);
    let modifiers: number = this.modifierByte();
    if (mapping.extended) {
      modifiers |= MlVirtualKey.MODIFIER_EXTENDED;
    }
    return mlAccepted(this.port.keyVirtual(mapping.vk, down, modifiers));
  }

  metaKey(down: boolean): boolean {
    this.trackModifier(ML_VK_META, down);
    return mlAccepted(this.port.keyVirtual(ML_VK_META, down,
      this.modifierByte() | MlVirtualKey.MODIFIER_EXTENDED));
  }

  unicode(code: number, down: boolean): boolean {
    if (down && code > 0) {
      return this.textRun(String.fromCharCode(code));
    }
    return true;
  }

  // One packet for a whole run: the library flushes and polls per packet, so a burst has to arrive
  // as few events as the pipeline can batch it into.
  textRun(text: string): boolean {
    if (text.length === 0) {
      return true;
    }
    return mlAccepted(this.port.text(text));
  }

  lockState(_flags: number): boolean {
    // No lock-state PDU on Moonlight; the host tracks its own lock state.
    return true;
  }

  pointerMove(x: number, y: number): boolean {
    return mlAccepted(this.port.mousePosition(x, y, this.referenceWidth, this.referenceHeight));
  }

  // Verbatim device counts: the host applies its own pointer curve (see the library's 1ms batching).
  pointerMoveRelative(deltaX: number, deltaY: number): boolean {
    if (deltaX === 0 && deltaY === 0) {
      return true;
    }
    return mlAccepted(this.port.mouseMove(MoonlightInputSender.clampShort(deltaX),
      MoonlightInputSender.clampShort(deltaY)));
  }

  touch(phase: number, pointerId: number, x: number, y: number, pressure: number): boolean {
    const normalizedX: number = this.referenceWidth > 0 ? x / this.referenceWidth : 0;
    const normalizedY: number = this.referenceHeight > 0 ? y / this.referenceHeight : 0;
    return mlAccepted(this.port.touch(MoonlightInputSender.mlTouchEvent(phase), pointerId,
      normalizedX, normalizedY, pressure));
  }

  scroll(vertical: number, horizontal: number, _x: number, _y: number): boolean {
    let accepted: boolean = true;
    if (vertical !== 0) {
      accepted = mlAccepted(this.port.highResScroll(MoonlightInputSender.clampShort(vertical)));
    }
    if (horizontal !== 0) {
      accepted = mlAccepted(this.port.highResHScroll(MoonlightInputSender.clampShort(horizontal)))
        && accepted;
    }
    return accepted;
  }

  touchpadFrame(contacts: TouchpadContact[], buttonState: number, widthMm: number,
    heightMm: number): boolean {
    // Hosts that do not advertise the extension ignore the packet; that is a by-design refusal,
    // not a drop, so it is reported as accepted.
    if (contacts.length === 0) {
      return true;
    }
    const types: number[] = [];
    const ids: number[] = [];
    const xs: number[] = [];
    const ys: number[] = [];
    const ps: number[] = [];
    for (let i = 0; i < contacts.length; i++) {
      const contact: TouchpadContact = contacts[i];
      types.push(MoonlightInputSender.mlTouchEvent(contact.phase));
      ids.push(contact.pointerId);
      xs.push(contact.x);
      ys.push(contact.y);
      ps.push(contact.pressure);
    }
    return mlAccepted(this.port.touchpad(types, ids, xs, ys, ps, widthMm, heightMm, buttonState));
  }

  button(action: number, button: number, x: number, y: number): boolean {
    const mlButton: number = MoonlightInputSender.mlButton(button);
    if (mlButton === 0) {
      return true;
    }
    // Relative mode: a click must not drag the host cursor back to the local pointer's coordinates.
    if (!this.relative) {
      this.port.mousePosition(x, y, this.referenceWidth, this.referenceHeight);
    }
    const mlAction: number = action === InputAction.PRESS
      ? MlVirtualKey.ACTION_PRESS : MlVirtualKey.ACTION_RELEASE;
    return mlAccepted(this.port.mouseButton(mlAction, mlButton));
  }

  // One controller slot's whole state (LiSendMultiControllerEvent); triggers are 0..255 and
  // sticks -32767..32767 with +Y up.
  controllerState(controllerNumber: number, activeGamepadMask: number, buttonFlags: number,
    leftTrigger: number, rightTrigger: number, leftStickX: number, leftStickY: number,
    rightStickX: number, rightStickY: number): void {
    this.port.controller(controllerNumber, activeGamepadMask, buttonFlags, leftTrigger, rightTrigger,
      leftStickX, leftStickY, rightStickX, rightStickY);
  }

  // Declares one attached controller's type / buttons / capabilities (LiSendControllerArrivalEvent).
  controllerArrival(controllerNumber: number, activeGamepadMask: number, type: number,
    supportedButtonFlags: number, capabilities: number): void {
    this.port.controllerArrival(controllerNumber, activeGamepadMask, type, supportedButtonFlags,
      capabilities);
  }

  private trackModifier(vk: number, down: boolean): void {
    if (MlVirtualKey.modifierFor(vk) === 0) {
      return;
    }
    if (down) {
      this.heldModifiers.add(vk);
    } else {
      this.heldModifiers.delete(vk);
    }
  }

  private modifierByte(): number {
    let modifiers: number = 0;
    this.heldModifiers.forEach((vk: number) => {
      modifiers |= MlVirtualKey.modifierFor(vk);
    });
    return modifiers;
  }

  private static mlButton(button: number): number {
    if (button === InputButton.LEFT) {
      return MlVirtualKey.BUTTON_LEFT;
    }
    if (button === InputButton.MIDDLE) {
      return MlVirtualKey.BUTTON_MIDDLE;
    }
    if (button === InputButton.RIGHT) {
      return MlVirtualKey.BUTTON_RIGHT;
    }
    if (button === InputButton.X1) {
      return MlVirtualKey.BUTTON_X1;
    }
    if (button === InputButton.X2) {
      return MlVirtualKey.BUTTON_X2;
    }
    return 0;
  }

  private static mlTouchEvent(phase: number): number {
    if (phase === TouchPhase.DOWN) {
      return 0x01; // LI_TOUCH_EVENT_DOWN
    }
    if (phase === TouchPhase.UP) {
      return 0x02; // LI_TOUCH_EVENT_UP
    }
    if (phase === TouchPhase.CANCEL) {
      return 0x04; // LI_TOUCH_EVENT_CANCEL
    }
    return 0x03; // LI_TOUCH_EVENT_MOVE
  }

  private static clampShort(value: number): number {
    return Math.max(-32768, Math.min(32767, Math.round(value)));
  }
}
