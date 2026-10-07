/*
 * HmRdp - Moonlight pairing prompt.
 *
 * The pairing PIN is a lightweight popup over the calling window: the client
 * generated the PIN, the user types it on the host (Sunshine), and the dialog
 * only reports whether the user says it is done. Shared by every entry point
 * that can start a pairing (the list, the editor and the app page).
 */
import { UIContext } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpPairing';

/** Shows the PIN and resolves true once the user confirms it on the host. */
export async function promptPairingPin(ui: UIContext, pin: string): Promise<boolean> {
  try {
    const response = await ui.getPromptAction().showDialog({
      title: '与主机配对',
      message: `请在主机（Sunshine）上输入以下 PIN：\n\n${pin}`,
      buttons: [
        { text: '取消', color: '#666666' },
        { text: '已输入', color: '#0A59F7' }
      ]
    });
    return response.index === 1;
  } catch (error) {
    hilog.error(DOMAIN, TAG, 'pairing dialog failed: %{public}s', JSON.stringify(error));
    return false;
  }
}
