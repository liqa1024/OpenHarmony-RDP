import { display } from '@kit.ArkUI';

export function defaultDisplay(): display.Display | undefined {
  try {
    return display.getDefaultDisplaySync();
  } catch (error) {
    return undefined;
  }
}
