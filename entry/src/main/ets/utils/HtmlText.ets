// HTML -> plain text for the clipboard's text fallback, used when the source
// side did not hand us its own plain rendition. Done by the platform's HTML
// converter (ArkUI styled string, Web-based under the hood) rather than a
// hand-rolled tag scanner: it is stage-model only, so it must run on the UI
// thread, never in the background worker.
export class HtmlText {
  static async toPlainText(html: string): Promise<string> {
    if (html.length === 0) {
      return '';
    }
    try {
      const styled: StyledString = await StyledString.fromHtml(html);
      return styled.getString();
    } catch (error) {
      return '';
    }
  }
}
