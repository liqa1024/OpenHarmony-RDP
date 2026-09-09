/*
 * HmRdp - clipboard redirection watcher.
 *
 * Watches the system pasteboard while a session is open and forwards text
 * changes to the owning native session. Kept out of the page because
 * SessionPage.aboutToDisappear() does not run when the window is closed; the
 * ability tears the watcher down explicitly instead.
 */
import { BusinessError, pasteboard } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpClipboard';

class SyncEntry {
  send: (text: string) => void = (text: string): void => {};
  handler: () => void = (): void => {};
  // Text we last wrote from the remote; used to ignore the 'update' event our
  // own write triggers so it is never echoed back.
  lastRemoteText: string = '';
}

export class ClipboardSync {
  private static entries: Map<string, SyncEntry> = new Map<string, SyncEntry>();

  /** Starts watching the pasteboard for the given session. Idempotent. */
  static start(key: string, send: (text: string) => void): void {
    if (key.length === 0 || ClipboardSync.entries.has(key)) {
      return;
    }
    const entry: SyncEntry = new SyncEntry();
    entry.send = send;
    entry.handler = (): void => {
      ClipboardSync.push(key);
    };
    try {
      pasteboard.getSystemPasteboard().on('update', entry.handler);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'observe failed: %{public}s', JSON.stringify(error));
      return;
    }
    ClipboardSync.entries.set(key, entry);
    // Seed whatever is already on the clipboard so the first remote paste works.
    ClipboardSync.push(key);
  }

  static stop(key: string): void {
    const entry: SyncEntry | undefined = ClipboardSync.entries.get(key);
    if (entry === undefined) {
      return;
    }
    try {
      pasteboard.getSystemPasteboard().off('update', entry.handler);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'unobserve failed: %{public}s', JSON.stringify(error));
    }
    ClipboardSync.entries.delete(key);
  }

  /** Records text written from the remote so it is not sent back. */
  static markRemoteText(key: string, text: string): void {
    const entry: SyncEntry | undefined = ClipboardSync.entries.get(key);
    if (entry !== undefined) {
      entry.lastRemoteText = text;
    }
  }

  private static push(key: string): void {
    const entry: SyncEntry | undefined = ClipboardSync.entries.get(key);
    if (entry === undefined) {
      return;
    }
    pasteboard.getSystemPasteboard().getData()
      .then((data: pasteboard.PasteData) => {
        let text: string = '';
        try {
          text = data.getPrimaryText();
        } catch (error) {
          text = '';
        }
        if (text.length === 0 || text === entry.lastRemoteText) {
          return;
        }
        hilog.info(DOMAIN, TAG, 'local clipboard -> remote: %{public}d chars', text.length);
        entry.send(text);
      })
      .catch((error: BusinessError) => {
        hilog.warn(DOMAIN, TAG, 'read pasteboard failed: %{public}s', JSON.stringify(error));
      });
  }
}
