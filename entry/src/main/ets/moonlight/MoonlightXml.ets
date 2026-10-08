/*
 * HmRdp - Moonlight XML parsing (ArkTS side).
 *
 * All GameStream XML is parsed here with the platform parser (`@ohos.xml`), the
 * same way the official clients keep the protocol and its XML in one layer
 * (doc_agent/moonlight-native.md §4). The native side returns raw HTTP bodies.
 */
import { util, xml } from '@kit.ArkTS';
import { NvApp } from './MoonlightModels';

function parse(xmlText: string, tokenCallback: (eventType: xml.EventType,
  info: xml.ParseInfo) => boolean): void {
  const bytes: Uint8Array = new util.TextEncoder().encodeInto(xmlText);
  const parser: xml.XmlPullParser =
    new xml.XmlPullParser(bytes.buffer as object as ArrayBuffer, 'UTF-8');
  const options: xml.ParseOptions = {
    supportDoctype: true,
    ignoreNameSpace: true,
    tokenValueCallbackFunction: tokenCallback
  };
  parser.parseXml(options);
}

export class MoonlightXml {
  /**
   * The `<root status_code="...">` value, or -1 when absent. GFE can return
   * 0xFFFFFFFF, which is parsed as a plain (possibly negative) integer.
   */
  static statusCode(xmlText: string): number {
    const bytes: Uint8Array = new util.TextEncoder().encodeInto(xmlText);
    const parser: xml.XmlPullParser =
      new xml.XmlPullParser(bytes.buffer as object as ArrayBuffer, 'UTF-8');
    let code: number = -1;
    const callback = (name: string, value: string): boolean => {
      if (name === 'status_code') {
        code = Number.parseInt(value, 10);
        return false;
      }
      return true;
    };
    const options: xml.ParseOptions = {
      supportDoctype: true,
      ignoreNameSpace: true,
      attributeValueCallbackFunction: callback
    };
    parser.parseXml(options);
    return code;
  }

  /** The text of the first `tag`, or "" when the tag is absent or empty. */
  static read(xmlText: string, tag: string): string {
    let current: string = '';
    let value: string = '';
    parse(xmlText, (eventType: xml.EventType, info: xml.ParseInfo): boolean => {
      if (eventType === xml.EventType.START_TAG) {
        current = info.getName();
      } else if (eventType === xml.EventType.TEXT && current === tag) {
        value = info.getText();
        return false;
      }
      return true;
    });
    return value;
  }

  /** The text of `tag`, or undefined when the tag is absent. */
  static readOptional(xmlText: string, tag: string): string | undefined {
    let current: string = '';
    let value: string | undefined = undefined;
    parse(xmlText, (eventType: xml.EventType, info: xml.ParseInfo): boolean => {
      if (eventType === xml.EventType.START_TAG) {
        current = info.getName();
      } else if (eventType === xml.EventType.TEXT && current === tag) {
        value = info.getText();
        return false;
      }
      return true;
    });
    return value;
  }

  /** Parses `tag` as a base-10 number, or `fallback` when absent/malformed. */
  static readNumber(xmlText: string, tag: string, fallback: number): number {
    const raw: string = MoonlightXml.read(xmlText, tag);
    if (raw.length === 0) {
      return fallback;
    }
    const value: number = Number.parseInt(raw, 10);
    return Number.isNaN(value) ? fallback : value;
  }

  /** Parses every `<App>` of an `/applist` response. */
  static parseAppList(xmlText: string): NvApp[] {
    const apps: NvApp[] = [];
    let current: string = '';
    let app: NvApp | undefined = undefined;
    parse(xmlText, (eventType: xml.EventType, info: xml.ParseInfo): boolean => {
      if (eventType === xml.EventType.START_TAG) {
        current = info.getName();
        if (current === 'App') {
          app = new NvApp();
          apps.push(app);
        }
        return true;
      }
      if (eventType === xml.EventType.TEXT && app !== undefined) {
        if (current === 'AppTitle') {
          app.name = info.getText();
        } else if (current === 'ID') {
          app.id = Number.parseInt(info.getText(), 10);
        }
      }
      return true;
    });
    // Drop entries that never completed (mirroring the official client).
    const result: NvApp[] = [];
    for (const entry of apps) {
      if (entry.name.length > 0) {
        result.push(entry);
      }
    }
    return result;
  }
}
