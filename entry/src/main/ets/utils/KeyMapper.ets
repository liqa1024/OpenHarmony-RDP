/*
 * HmRdp - maps HarmonyOS multimodal key codes to RDP (PS/2 set 1) scancodes.
 */

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

const KEYCODE_0: number = 2000;
const KEYCODE_9: number = 2009;
const KEYCODE_A: number = 2017;
const KEYCODE_Z: number = 2042;

const DIGIT_SCAN: number[] = [0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A];
const LETTER_SCAN: number[] = [
  0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
  0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C
];

export class KeyMapper {
  static map(keyCode: number): RdpScanCode | undefined {
    if (keyCode >= KEYCODE_0 && keyCode <= KEYCODE_9) {
      return RdpScanCode.of(DIGIT_SCAN[keyCode - KEYCODE_0], false);
    }
    if (keyCode >= KEYCODE_A && keyCode <= KEYCODE_Z) {
      return RdpScanCode.of(LETTER_SCAN[keyCode - KEYCODE_A], false);
    }
    switch (keyCode) {
      case 2012: return RdpScanCode.of(0x48, true); // DPAD_UP
      case 2013: return RdpScanCode.of(0x50, true); // DPAD_DOWN
      case 2014: return RdpScanCode.of(0x4B, true); // DPAD_LEFT
      case 2015: return RdpScanCode.of(0x4D, true); // DPAD_RIGHT
      case 2043: return RdpScanCode.of(0x33, false); // comma
      case 2044: return RdpScanCode.of(0x34, false); // period
      case 2045: return RdpScanCode.of(0x38, false); // alt left
      case 2046: return RdpScanCode.of(0x38, true); // alt right
      case 2047: return RdpScanCode.of(0x2A, false); // shift left
      case 2048: return RdpScanCode.of(0x36, false); // shift right
      case 2049: return RdpScanCode.of(0x0F, false); // tab
      case 2050: return RdpScanCode.of(0x39, false); // space
      case 2054: return RdpScanCode.of(0x1C, false); // enter
      case 2055: return RdpScanCode.of(0x0E, false); // backspace
      case 2056: return RdpScanCode.of(0x29, false); // grave
      case 2057: return RdpScanCode.of(0x0C, false); // minus
      case 2058: return RdpScanCode.of(0x0D, false); // equals
      case 2059: return RdpScanCode.of(0x1A, false); // left bracket
      case 2060: return RdpScanCode.of(0x1B, false); // right bracket
      case 2061: return RdpScanCode.of(0x2B, false); // backslash
      case 2062: return RdpScanCode.of(0x27, false); // semicolon
      case 2063: return RdpScanCode.of(0x28, false); // apostrophe
      case 2064: return RdpScanCode.of(0x35, false); // slash
      case 2067: return RdpScanCode.of(0x5D, true); // menu
      case 2068: return RdpScanCode.of(0x49, true); // page up
      case 2069: return RdpScanCode.of(0x51, true); // page down
      case 2070: return RdpScanCode.of(0x01, false); // escape
      case 2071: return RdpScanCode.of(0x53, true); // forward delete
      case 2072: return RdpScanCode.of(0x1D, false); // ctrl left
      case 2073: return RdpScanCode.of(0x1D, true); // ctrl right
      case 2074: return RdpScanCode.of(0x3A, false); // caps lock
      case 2075: return RdpScanCode.of(0x46, false); // scroll lock
      case 2076: return RdpScanCode.of(0x5B, true); // meta left
      case 2077: return RdpScanCode.of(0x5C, true); // meta right
      case 2081: return RdpScanCode.of(0x47, true); // home
      case 2082: return RdpScanCode.of(0x4F, true); // end
      case 2083: return RdpScanCode.of(0x52, true); // insert
      case 2090: return RdpScanCode.of(0x3B, false); // F1
      case 2091: return RdpScanCode.of(0x3C, false);
      case 2092: return RdpScanCode.of(0x3D, false);
      case 2093: return RdpScanCode.of(0x3E, false);
      case 2094: return RdpScanCode.of(0x3F, false);
      case 2095: return RdpScanCode.of(0x40, false);
      case 2096: return RdpScanCode.of(0x41, false);
      case 2097: return RdpScanCode.of(0x42, false);
      case 2098: return RdpScanCode.of(0x43, false);
      case 2099: return RdpScanCode.of(0x44, false);
      case 2100: return RdpScanCode.of(0x57, false); // F11
      case 2101: return RdpScanCode.of(0x58, false); // F12
      default: return undefined;
    }
  }
}
