import { fileIo, fileUri } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpBackground';
const FALLBACK_SUFFIX: string = '.jpg';
const MAX_SUFFIX_LENGTH: number = 6;

// The picked image is copied into the app sandbox: a picker URI is only granted for
// the session, while the main page has to read the wallpaper on every launch.
export class BackgroundImage {
  static async importFromUri(filesDir: string, uri: string): Promise<string> {
    if (filesDir.length === 0 || uri.length === 0) {
      return '';
    }
    // A fresh name per selection: an unchanged path would let the Image cache keep
    // showing the previous wallpaper after the file is overwritten.
    const dest: string = filesDir + '/background_' + Date.now() + BackgroundImage.suffixOf(uri);
    let src: fileIo.File | undefined = undefined;
    try {
      src = fileIo.openSync(uri, fileIo.OpenMode.READ_ONLY);
      await fileIo.copyFile(src.fd, dest);
      return dest;
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'copy background failed: %{public}s', JSON.stringify(error));
      return '';
    } finally {
      if (src !== undefined) {
        try {
          fileIo.closeSync(src);
        } catch (error) {
          hilog.warn(DOMAIN, TAG, 'close background source failed: %{public}s',
            JSON.stringify(error));
        }
      }
    }
  }

  static remove(path: string): void {
    if (path.length === 0) {
      return;
    }
    try {
      if (fileIo.accessSync(path)) {
        fileIo.unlinkSync(path);
      }
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'remove background failed: %{public}s', JSON.stringify(error));
    }
  }

  // Image takes a sandbox URI, not a bare sandbox path.
  static uriOf(path: string): string {
    if (path.length === 0) {
      return '';
    }
    try {
      return fileUri.getUriFromPath(path);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'resolve background uri failed: %{public}s', JSON.stringify(error));
      return '';
    }
  }

  private static suffixOf(uri: string): string {
    const clean: string = uri.split('?')[0];
    const name: string = clean.substring(clean.lastIndexOf('/') + 1);
    const dot: number = name.lastIndexOf('.');
    if (dot < 0) {
      return FALLBACK_SUFFIX;
    }
    const suffix: string = name.substring(dot + 1).toLowerCase();
    if (suffix.length === 0 || suffix.length > MAX_SUFFIX_LENGTH) {
      return FALLBACK_SUFFIX;
    }
    for (let i = 0; i < suffix.length; i++) {
      const code: number = suffix.charCodeAt(i);
      if ((code < 97 || code > 122) && (code < 48 || code > 57)) {
        return FALLBACK_SUFFIX;
      }
    }
    return '.' + suffix;
  }
}
