import { KeyMapping } from '../../utils/KeyMapper';
import { InputAction, InputSink } from './InputSink';

// Ledger of the inputs still held down, kept by remote identity so a blur can lift every one of
// them. Any new input path that can hold a key/button must record here.
export class HeldInputs {
  private keys: Map<string, KeyMapping> = new Map<string, KeyMapping>();
  private unicode: Set<number> = new Set<number>();
  private meta: boolean = false;
  private buttons: Set<number> = new Set<number>();

  trackKey(keyCode: number, mapping: KeyMapping, down: boolean): void {
    const key: string = keyCode.toString();
    if (down) {
      if (!this.keys.has(key)) {
        this.keys.set(key, mapping);
      }
    } else {
      this.keys.delete(key);
    }
  }

  trackUnicode(code: number, down: boolean): void {
    if (down) {
      this.unicode.add(code);
    } else {
      this.unicode.delete(code);
    }
  }

  trackMeta(down: boolean): void {
    this.meta = down;
  }

  pressButton(button: number): void {
    this.buttons.add(button);
  }

  releaseButton(button: number): void {
    this.buttons.delete(button);
  }

  releaseAll(sink: InputSink, x: number, y: number): void {
    this.keys.forEach((mapping: KeyMapping): void => {
      sink.key(mapping, false);
    });
    this.keys.clear();
    this.unicode.forEach((code: number): void => {
      sink.unicode(code, false);
    });
    this.unicode.clear();
    if (this.meta) {
      sink.metaKey(false);
      this.meta = false;
    }
    this.buttons.forEach((button: number): void => {
      sink.button(InputAction.RELEASE, button, x, y);
    });
    this.buttons.clear();
  }
}
