export class MlVirtualKey {
  static readonly ACTION_PRESS: number = 0x07;
  static readonly ACTION_RELEASE: number = 0x08;
  static readonly BUTTON_LEFT: number = 0x01;
  static readonly BUTTON_MIDDLE: number = 0x02;
  static readonly BUTTON_RIGHT: number = 0x03;
  static readonly BUTTON_X1: number = 0x04;
  static readonly BUTTON_X2: number = 0x05;
  static readonly MODIFIER_SHIFT: number = 0x01;
  static readonly MODIFIER_CTRL: number = 0x02;
  static readonly MODIFIER_ALT: number = 0x04;
  static readonly MODIFIER_META: number = 0x08;
  // Marks an 0xE0-prefixed key; separates keys that share a VK (Enter / Numpad Enter).
  static readonly MODIFIER_EXTENDED: number = 0x10;

  static modifierFor(vk: number): number {
    if (vk === 0x10 || vk === 0xA0 || vk === 0xA1) {
      return MlVirtualKey.MODIFIER_SHIFT;
    }
    if (vk === 0x11 || vk === 0xA2 || vk === 0xA3) {
      return MlVirtualKey.MODIFIER_CTRL;
    }
    if (vk === 0x12 || vk === 0xA4 || vk === 0xA5) {
      return MlVirtualKey.MODIFIER_ALT;
    }
    if (vk === 0x5B || vk === 0x5C) {
      return MlVirtualKey.MODIFIER_META;
    }
    return 0;
  }
}
