import { RdpLockFlags } from '../model/RdpModels';

// RDP-only view: PS/2 set-1 scancode plus the 0xE0 extended flag.
export class RdpScanCode {
  scancode: number = 0;
  extended: boolean = false;

  static of(scancode: number, extended: boolean): RdpScanCode {
    const result: RdpScanCode = new RdpScanCode();
    result.scancode = scancode;
    result.extended = extended;
    return result;
  }
}

// One physical key described once for every protocol: RDP sends scancode+extended,
// Moonlight sends vk+extended; lock is the remote lock flag a lock key toggles.
export class KeyMapping {
  scancode: number = 0;
  vk: number = 0;
  extended: boolean = false;
  lock: number = 0;

  static of(scancode: number, vk: number, extended: boolean, lock: number): KeyMapping {
    const result: KeyMapping = new KeyMapping();
    result.scancode = scancode;
    result.vk = vk;
    result.extended = extended;
    result.lock = lock;
    return result;
  }
}

const KEYCODE_0: number = 2000;
const KEYCODE_9: number = 2009;
const KEYCODE_A: number = 2017;
const KEYCODE_Z: number = 2042;

const DIGIT_SCAN: number[] = [0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A];
const LETTER_SCAN: number[] = [
  0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
  0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C
];
// Numpad 0..9 share scancodes with the nav cluster but carry no extended flag.
const NUMPAD_SCAN: number[] = [0x52, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49];

const VK_0: number = 0x30;
const VK_A: number = 0x41;
const VK_F1: number = 0x70;
const VK_NUMPAD0: number = 0x60;

// A single source of truth for both protocols: one row per HarmonyOS keyCode.
export class KeyMapper {
  private static lookup(keyCode: number): KeyMapping | undefined {
    if (keyCode >= KEYCODE_0 && keyCode <= KEYCODE_9) {
      return KeyMapping.of(DIGIT_SCAN[keyCode - KEYCODE_0], VK_0 + (keyCode - KEYCODE_0), false, 0);
    }
    if (keyCode >= KEYCODE_A && keyCode <= KEYCODE_Z) {
      return KeyMapping.of(LETTER_SCAN[keyCode - KEYCODE_A], VK_A + (keyCode - KEYCODE_A), false, 0);
    }
    if (keyCode >= 2090 && keyCode <= 2101) {
      const f: number = keyCode - 2090;
      const scan: number = f === 10 ? 0x57 : (f === 11 ? 0x58 : 0x3B + f);
      return KeyMapping.of(scan, VK_F1 + f, false, 0);
    }
    if (keyCode >= 2103 && keyCode <= 2112) {
      const n: number = keyCode - 2103;
      return KeyMapping.of(NUMPAD_SCAN[n], VK_NUMPAD0 + n, false, 0);
    }
    switch (keyCode) {
      case 2012: return KeyMapping.of(0x48, 0x26, true, 0); // up
      case 2013: return KeyMapping.of(0x50, 0x28, true, 0); // down
      case 2014: return KeyMapping.of(0x4B, 0x25, true, 0); // left
      case 2015: return KeyMapping.of(0x4D, 0x27, true, 0); // right
      case 2043: return KeyMapping.of(0x33, 0xBC, false, 0); // comma
      case 2044: return KeyMapping.of(0x34, 0xBE, false, 0); // period
      case 2045: return KeyMapping.of(0x38, 0x12, false, 0); // alt left
      case 2046: return KeyMapping.of(0x38, 0x12, true, 0); // alt right
      case 2047: return KeyMapping.of(0x2A, 0x10, false, 0); // shift left
      case 2048: return KeyMapping.of(0x36, 0x10, false, 0); // shift right
      case 2049: return KeyMapping.of(0x0F, 0x09, false, 0); // tab
      case 2050: return KeyMapping.of(0x39, 0x20, false, 0); // space
      case 2054: return KeyMapping.of(0x1C, 0x0D, false, 0); // enter
      case 2055: return KeyMapping.of(0x0E, 0x08, false, 0); // backspace
      case 2056: return KeyMapping.of(0x29, 0xC0, false, 0); // grave
      case 2057: return KeyMapping.of(0x0C, 0xBD, false, 0); // minus
      case 2058: return KeyMapping.of(0x0D, 0xBB, false, 0); // equals
      case 2059: return KeyMapping.of(0x1A, 0xDB, false, 0); // left bracket
      case 2060: return KeyMapping.of(0x1B, 0xDD, false, 0); // right bracket
      case 2061: return KeyMapping.of(0x2B, 0xDC, false, 0); // backslash
      case 2062: return KeyMapping.of(0x27, 0xBA, false, 0); // semicolon
      case 2063: return KeyMapping.of(0x28, 0xDE, false, 0); // apostrophe
      case 2064: return KeyMapping.of(0x35, 0xBF, false, 0); // slash
      case 2067: return KeyMapping.of(0x5D, 0x5D, true, 0); // menu
      case 2068: return KeyMapping.of(0x49, 0x21, true, 0); // page up
      case 2069: return KeyMapping.of(0x51, 0x22, true, 0); // page down
      case 2070: return KeyMapping.of(0x01, 0x1B, false, 0); // escape
      case 2071: return KeyMapping.of(0x53, 0x2E, true, 0); // forward delete
      case 2072: return KeyMapping.of(0x1D, 0x11, false, 0); // ctrl left
      case 2073: return KeyMapping.of(0x1D, 0x11, true, 0); // ctrl right
      case 2074: return KeyMapping.of(0x3A, 0x14, false, RdpLockFlags.CAPS); // caps lock
      case 2075: return KeyMapping.of(0x46, 0x91, false, RdpLockFlags.SCROLL); // scroll lock
      case 2076: return KeyMapping.of(0x5B, 0x5B, true, 0); // meta left
      case 2077: return KeyMapping.of(0x5C, 0x5C, true, 0); // meta right
      case 2081: return KeyMapping.of(0x47, 0x24, true, 0); // home
      case 2082: return KeyMapping.of(0x4F, 0x23, true, 0); // end
      case 2083: return KeyMapping.of(0x52, 0x2D, true, 0); // insert
      case 2102: return KeyMapping.of(0x45, 0x90, false, RdpLockFlags.NUM); // num lock
      case 2113: return KeyMapping.of(0x35, 0x6F, true, 0); // numpad divide
      case 2114: return KeyMapping.of(0x37, 0x6A, false, 0); // numpad multiply
      case 2115: return KeyMapping.of(0x4A, 0x6D, false, 0); // numpad subtract
      case 2116: return KeyMapping.of(0x4E, 0x6B, false, 0); // numpad add
      case 2117: return KeyMapping.of(0x53, 0x6E, false, 0); // numpad dot
      case 2119: return KeyMapping.of(0x1C, 0x0D, true, 0); // numpad enter
      // Numpad equals (scancode 0x59) has no standard Windows VK; RDP only.
      case 2120: return KeyMapping.of(0x59, 0, false, 0);
      default: return undefined;
    }
  }

  static mapFull(keyCode: number): KeyMapping | undefined {
    return KeyMapper.lookup(keyCode);
  }

  static map(keyCode: number): RdpScanCode | undefined {
    const mapping: KeyMapping | undefined = KeyMapper.lookup(keyCode);
    if (mapping === undefined) {
      return undefined;
    }
    return RdpScanCode.of(mapping.scancode, mapping.extended);
  }

  static describe(keyCode: number): string {
    if (keyCode >= KEYCODE_0 && keyCode <= KEYCODE_9) {
      return (keyCode - KEYCODE_0).toString();
    }
    if (keyCode >= KEYCODE_A && keyCode <= KEYCODE_Z) {
      return String.fromCharCode(65 + (keyCode - KEYCODE_A));
    }
    if (keyCode >= 2090 && keyCode <= 2101) {
      return 'F' + (keyCode - 2089).toString();
    }
    switch (keyCode) {
      case 2012: return '方向键 上';
      case 2013: return '方向键 下';
      case 2014: return '方向键 左';
      case 2015: return '方向键 右';
      case 2043: return ',';
      case 2044: return '.';
      case 2045: return '左 Alt';
      case 2046: return '右 Alt';
      case 2047: return '左 Shift';
      case 2048: return '右 Shift';
      case 2049: return 'Tab';
      case 2050: return '空格';
      case 2054: return '回车';
      case 2055: return '退格';
      case 2056: return '`';
      case 2057: return '-';
      case 2058: return '=';
      case 2059: return '[';
      case 2060: return ']';
      case 2061: return '\\';
      case 2062: return ';';
      case 2063: return '\'';
      case 2064: return '/';
      case 2067: return '菜单键';
      case 2068: return 'Page Up';
      case 2069: return 'Page Down';
      case 2070: return 'Esc';
      case 2071: return 'Delete';
      case 2072: return '左 Ctrl';
      case 2073: return '右 Ctrl';
      case 2074: return 'Caps Lock';
      case 2075: return 'Scroll Lock';
      case 2076: return '左 Win';
      case 2077: return '右 Win';
      case 2081: return 'Home';
      case 2082: return 'End';
      case 2083: return 'Insert';
      case 2102: return 'Num Lock';
      default: return '按键 ' + keyCode.toString();
    }
  }
}
