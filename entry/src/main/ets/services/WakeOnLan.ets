import { socket } from '@kit.NetworkKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpWakeOnLan';
// A magic packet carries no destination identity, so it can only be broadcast.
const BROADCAST_ADDRESS: string = '255.255.255.255';
// 9 by convention, 7 is the legacy alias.
const WOL_PORTS: number[] = [9, 7];

export class WakeOnLan {
  // Accepts "AA:BB:CC:DD:EE:FF" with any separator (or none).
  private static parseMac(mac: string): number[] {
    const hex: string = mac.replace(/[^0-9a-fA-F]/g, '');
    if (hex.length !== 12) {
      return [];
    }
    const bytes: number[] = [];
    for (let i = 0; i < 12; i += 2) {
      const value: number = Number.parseInt(hex.substring(i, i + 2), 16);
      if (Number.isNaN(value)) {
        return [];
      }
      bytes.push(value);
    }
    return bytes;
  }

  static magicPacket(mac: string): ArrayBuffer | undefined {
    const bytes: number[] = WakeOnLan.parseMac(mac);
    if (bytes.length !== 6) {
      return undefined;
    }
    const packet: ArrayBuffer = new ArrayBuffer(6 + 16 * 6);
    const view: Uint8Array = new Uint8Array(packet);
    view.fill(0xFF, 0, 6);
    for (let i = 0; i < 16; i++) {
      for (let j = 0; j < 6; j++) {
        view[6 + i * 6 + j] = bytes[j];
      }
    }
    return packet;
  }

  // True only means the datagrams were sent; the host may still ignore them.
  static async wake(mac: string): Promise<boolean> {
    const packet: ArrayBuffer | undefined = WakeOnLan.magicPacket(mac);
    if (packet === undefined) {
      hilog.warn(DOMAIN, TAG, 'invalid MAC, cannot build magic packet');
      return false;
    }
    const udp: socket.UDPSocket = socket.constructUDPSocketInstance();
    try {
      const bindAddress: socket.NetAddress = { address: '0.0.0.0', port: 0 };
      await udp.bind(bindAddress);
      const extra: socket.UDPExtraOptions = { broadcast: true };
      await udp.setExtraOptions(extra);
      for (let i = 0; i < WOL_PORTS.length; i++) {
        const target: socket.NetAddress = { address: BROADCAST_ADDRESS, port: WOL_PORTS[i] };
        await udp.send({ data: packet, address: target });
      }
      return true;
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'wake failed: %{public}s', JSON.stringify(error));
      return false;
    } finally {
      WakeOnLan.close(udp);
    }
  }

  private static close(udp: socket.UDPSocket): void {
    udp.close().catch((error: BusinessError): void => {
      hilog.warn(DOMAIN, TAG, 'close failed: %{public}s', JSON.stringify(error));
    });
  }
}
