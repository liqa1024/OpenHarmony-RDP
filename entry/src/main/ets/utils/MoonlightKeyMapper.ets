export class MlVirtualKey {
  static readonly ACTION_PRESS: number = 0x07;
  static readonly ACTION_RELEASE: number = 0x08;
  static readonly BUTTON_LEFT: number = 0x01;
  static readonly BUTTON_MIDDLE: number = 0x02;
  static readonly BUTTON_RIGHT: number = 0x03;
  static readonly KEY_DOWN: number = 0x03;
  static readonly KEY_UP: number = 0x04;
  static readonly MODIFIER_SHIFT: number = 0x01;
  static readonly MODIFIER_CTRL: number = 0x02;
  static readonly MODIFIER_ALT: number = 0x04;
  static readonly MODIFIER_META: number = 0x08;

  static buttonFor(button: MouseButton): number {
    if (button === MouseButton.Left) {
      return MlVirtualKey.BUTTON_LEFT;
    }
    if (button === MouseButton.Middle) {
      return MlVirtualKey.BUTTON_MIDDLE;
    }
    if (button === MouseButton.Right) {
      return MlVirtualKey.BUTTON_RIGHT;
    }
    return 0;
  }

  static modifierFor(vk: number): number {
    if (vk === 0x10) {
      return MlVirtualKey.MODIFIER_SHIFT;
    }
    if (vk === 0x11) {
      return MlVirtualKey.MODIFIER_CTRL;
    }
    if (vk === 0x12) {
      return MlVirtualKey.MODIFIER_ALT;
    }
    if (vk === 0x5B || vk === 0x5C) {
      return MlVirtualKey.MODIFIER_META;
    }
    return 0;
  }

  static map(keyCode: number): number {
    if (keyCode >= 2000 && keyCode <= 2009) {
      return 0x30 + (keyCode - 2000);
    }
    if (keyCode >= 2017 && keyCode <= 2042) {
      return 0x41 + (keyCode - 2017);
    }
    switch (keyCode) {
      case 2012: return 0x26; // up
      case 2013: return 0x28; // down
      case 2014: return 0x25; // left
      case 2015: return 0x27; // right
      case 2043: return 0xBC; // comma
      case 2044: return 0xBE; // period
      case 2045: return 0x12; // alt left
      case 2046: return 0x12; // alt right
      case 2047: return 0x10; // shift left
      case 2048: return 0x10; // shift right
      case 2049: return 0x09; // tab
      case 2050: return 0x20; // space
      case 2054: return 0x0D; // enter
      case 2055: return 0x08; // backspace
      case 2056: return 0xC0; // grave
      case 2057: return 0xBD; // minus
      case 2058: return 0xBB; // equals
      case 2059: return 0xDB; // left bracket
      case 2060: return 0xDD; // right bracket
      case 2061: return 0xDC; // backslash
      case 2062: return 0xBA; // semicolon
      case 2063: return 0xDE; // apostrophe
      case 2064: return 0xBF; // slash
      case 2067: return 0x5D; // menu
      case 2068: return 0x21; // page up
      case 2069: return 0x22; // page down
      case 2070: return 0x1B; // escape
      case 2071: return 0x2E; // forward delete
      case 2072: return 0x11; // ctrl left
      case 2073: return 0x11; // ctrl right
      case 2074: return 0x14; // caps lock
      case 2076: return 0x5B; // meta left
      case 2077: return 0x5C; // meta right
      case 2081: return 0x24; // home
      case 2082: return 0x23; // end
      case 2083: return 0x2D; // insert
      case 2090: return 0x70; // F1
      case 2091: return 0x71;
      case 2092: return 0x72;
      case 2093: return 0x73;
      case 2094: return 0x74;
      case 2095: return 0x75;
      case 2096: return 0x76;
      case 2097: return 0x77;
      case 2098: return 0x78;
      case 2099: return 0x79;
      case 2100: return 0x7A; // F11
      case 2101: return 0x7B; // F12
      case 2103: return 0x60; // numpad 0
      case 2104: return 0x61;
      case 2105: return 0x62;
      case 2106: return 0x63;
      case 2107: return 0x64;
      case 2108: return 0x65;
      case 2109: return 0x66;
      case 2110: return 0x67;
      case 2111: return 0x68;
      case 2112: return 0x69; // numpad 9
      case 2113: return 0x6F; // numpad divide
      case 2114: return 0x6A; // numpad multiply
      case 2115: return 0x6D; // numpad subtract
      case 2116: return 0x6B; // numpad add
      case 2117: return 0x6E; // numpad dot
      case 2119: return 0x0D; // numpad enter
      default: return -1;
    }
  }
}
