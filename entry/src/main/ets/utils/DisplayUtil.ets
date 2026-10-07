/*
 * HmRdp - default display query.
 *
 * A fault-tolerant lookup shared by the pages and by the window / settings
 * services (which need the screen bounds); it never throws to the caller.
 */
import { display } from '@kit.ArkUI';

export function defaultDisplay(): display.Display | undefined {
  try {
    return display.getDefaultDisplaySync();
  } catch (error) {
    return undefined;
  }
}
