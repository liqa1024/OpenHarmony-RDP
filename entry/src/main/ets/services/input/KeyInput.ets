import { KeyMapping, KeyMapper } from '../../utils/KeyMapper';
import { RdpLockFlags } from '../../model/RdpModels';
import { HeldInputs } from './HeldInputs';
import { InputSink } from './InputSink';

// Keyboard: scancode/Unicode forwarding, the Win-key substitute, and the local lock-state
// synchronize ordering. Held state is recorded into HeldInputs so a blur can lift it.
export class KeyInput {
  private sink: InputSink;
  private held: HeldInputs;
  private winKeySubstitute: number = 0;
  private interceptActive: boolean = false;

  constructor(sink: InputSink, held: HeldInputs) {
    this.sink = sink;
    this.held = held;
  }

  setWinKeySubstitute(keyCode: number): void {
    this.winKeySubstitute = keyCode;
  }

  setInterceptActive(active: boolean): void {
    this.interceptActive = active;
  }

  handleKey(event: KeyEvent): void {
    if (this.interceptActive) {
      return;
    }
    const down: boolean = event.type === KeyType.Down;
    // The real Win key is owned by the system; Win for the remote comes from the substitute key.
    if (event.keyCode === 2076 || event.keyCode === 2077) {
      return;
    }
    const mapping: KeyMapping | undefined = KeyMapper.mapFull(event.keyCode);
    const lockKey: boolean = mapping !== undefined && mapping.lock !== 0;
    // A non-lock key is synced before itself so the character gets the right case; a lock key after, since its press toggles the remote.
    if (!lockKey) {
      this.syncLocalLocks(event);
    }
    if (this.winKeySubstitute !== 0 && event.keyCode === this.winKeySubstitute) {
      this.held.trackMeta(down);
      this.sink.metaKey(down);
      return;
    }
    if (mapping !== undefined) {
      const repeat: boolean = down && this.held.hasKey(event.keyCode);
      this.held.trackKey(event.keyCode, mapping, down);
      if (repeat) {
        this.sink.keyRepeat(mapping);
      } else {
        this.sink.key(mapping, down);
      }
      if (lockKey) {
        this.syncLocalLocks(event);
      }
      return;
    }
    const text: string = event.keyText;
    if (text.length === 1) {
      const code: number = text.charCodeAt(0);
      if (code >= 0x20) {
        this.held.trackUnicode(code, down);
        this.sink.unicode(code, down);
      }
    }
  }

  // A key delivered by the system key interceptor; unlike handleKey it forwards the real Win key.
  handleInterceptedKey(keyCode: number, down: boolean): void {
    const mapping: KeyMapping | undefined = KeyMapper.mapFull(keyCode);
    if (mapping === undefined) {
      return;
    }
    const repeat: boolean = down && this.held.hasKey(keyCode);
    this.held.trackKey(keyCode, mapping, down);
    if (repeat) {
      this.sink.keyRepeat(mapping);
      return;
    }
    this.sink.key(mapping, down);
  }

  private syncLocalLocks(event: KeyEvent): void {
    const caps: boolean | undefined = event.isCapsLockOn;
    const num: boolean | undefined = event.isNumLockOn;
    const scroll: boolean | undefined = event.isScrollLockOn;
    if (caps === undefined || num === undefined || scroll === undefined) {
      return;
    }
    let flags: number = 0;
    if (scroll) {
      flags |= RdpLockFlags.SCROLL;
    }
    if (num) {
      flags |= RdpLockFlags.NUM;
    }
    if (caps) {
      flags |= RdpLockFlags.CAPS;
    }
    this.sink.lockState(flags);
  }
}
