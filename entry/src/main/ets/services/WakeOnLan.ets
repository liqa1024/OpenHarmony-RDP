import { connection, socket } from '@kit.NetworkKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpWakeOnLan';
// A magic packet carries no destination identity, so it can only be broadcast.
const BROADCAST_ADDRESS: string = '255.255.255.255';
// 9 by convention, 7 is the legacy alias.
const WOL_PORTS: number[] = [9, 7];

// Whether this client can put a broadcast on the host's own L2 segment. The magic
// packet is a limited broadcast, which no router forwards, so a WoL-armed NIC and a
// dead one are both silent here: off-LAN only rules out the send path, not the NIC.
export enum WakeReachability {
  Unknown = 0,
  SameLan = 1,
  OffLan = 2
}

export class WakeSendResult {
  sent: boolean = false;
  // The socket was pinned to the host's LAN; otherwise it used the default route.
  bound: boolean = false;
  ports: number[] = [];
}

class HostNetScan {
  handle: connection.NetHandle | undefined = undefined;
  // At least one local IPv4 was readable, so "no match" means off-LAN, not "unknown".
  inspected: boolean = false;
}

export class WakeOnLan {
  // Accepts "AA:BB:CC:DD:EE:FF" with any separator (or none).
  private static parseMac(mac: string): number[] {
    const hex: string = mac.replace(new RegExp('[^0-9a-fA-F]', 'g'), '');
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

  static async reachability(host: string): Promise<WakeReachability> {
    const scan: HostNetScan = await WakeOnLan.scanHostNet(host);
    if (scan.handle !== undefined) {
      return WakeReachability.SameLan;
    }
    return scan.inspected ? WakeReachability.OffLan : WakeReachability.Unknown;
  }

  // True only means the datagrams were sent; the host may still ignore them.
  static async wake(mac: string, host: string): Promise<WakeSendResult> {
    const result: WakeSendResult = new WakeSendResult();
    const packet: ArrayBuffer | undefined = WakeOnLan.magicPacket(mac);
    if (packet === undefined) {
      hilog.warn(DOMAIN, TAG, 'invalid MAC, cannot build magic packet');
      return result;
    }
    const udp: socket.UDPSocket = socket.constructUDPSocketInstance();
    try {
      const bindAddress: socket.NetAddress = { address: '0.0.0.0', port: 0 };
      await udp.bind(bindAddress);
      const extra: socket.UDPExtraOptions = { broadcast: true };
      await udp.setExtraOptions(extra);
      // Pin the socket to the host's LAN so the broadcast cannot leave through a
      // different interface (cellular) when the default net is not the LAN.
      const scan: HostNetScan = await WakeOnLan.scanHostNet(host);
      if (scan.handle !== undefined) {
        await scan.handle.bindSocket(udp);
        result.bound = true;
      }
      for (let i = 0; i < WOL_PORTS.length; i++) {
        const target: socket.NetAddress = { address: BROADCAST_ADDRESS, port: WOL_PORTS[i] };
        await udp.send({ data: packet, address: target });
        result.ports.push(WOL_PORTS[i]);
      }
      result.sent = result.ports.length > 0;
      return result;
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'wake failed: %{public}s', JSON.stringify(error));
      return result;
    } finally {
      WakeOnLan.close(udp);
    }
  }

  // Finds the connected net whose link address shares the host's subnet, if any.
  private static async scanHostNet(host: string): Promise<HostNetScan> {
    const scan: HostNetScan = new HostNetScan();
    const hostIp: number = await WakeOnLan.resolveIpv4(host);
    if (hostIp < 0) {
      return scan;
    }
    try {
      const nets: connection.NetHandle[] = await connection.getAllNets();
      for (let i = 0; i < nets.length; i++) {
        const props: connection.ConnectionProperties =
          await connection.getConnectionProperties(nets[i]);
        const links: connection.LinkAddress[] | undefined = props.linkAddresses;
        if (links === undefined) {
          continue;
        }
        for (let j = 0; j < links.length; j++) {
          const local: number = WakeOnLan.ipv4ToInt(links[j].address.address);
          if (local < 0) {
            continue;
          }
          scan.inspected = true;
          if (WakeOnLan.sameSubnet(hostIp, local, links[j].prefixLength)) {
            scan.handle = nets[i];
            return scan;
          }
        }
      }
    } catch (error) {
      hilog.debug(DOMAIN, TAG, 'scan host net failed: %{public}s', JSON.stringify(error));
    }
    return scan;
  }

  private static async resolveIpv4(host: string): Promise<number> {
    const direct: number = WakeOnLan.ipv4ToInt(host);
    if (direct >= 0) {
      return direct;
    }
    try {
      const addresses: connection.NetAddress[] = await connection.getAddressesByName(host);
      for (let i = 0; i < addresses.length; i++) {
        const value: number = WakeOnLan.ipv4ToInt(addresses[i].address);
        if (value >= 0) {
          return value;
        }
      }
    } catch (error) {
      hilog.debug(DOMAIN, TAG, 'resolve %{public}s failed: %{public}s', host, JSON.stringify(error));
    }
    return -1;
  }

  // Returns the IPv4 literal as an unsigned 32-bit number, or -1 if not IPv4.
  private static ipv4ToInt(text: string): number {
    const parts: string[] = text.split('.');
    if (parts.length !== 4) {
      return -1;
    }
    let value: number = 0;
    for (let i = 0; i < 4; i++) {
      if (!WakeOnLan.isDecimal(parts[i])) {
        return -1;
      }
      const octet: number = Number.parseInt(parts[i], 10);
      if (octet > 255) {
        return -1;
      }
      value = value * 256 + octet;
    }
    return value;
  }

  private static isDecimal(text: string): boolean {
    if (text.length === 0 || text.length > 3) {
      return false;
    }
    for (let i = 0; i < text.length; i++) {
      const code: number = text.charCodeAt(i);
      if (code < 0x30 || code > 0x39) {
        return false;
      }
    }
    return true;
  }

  private static sameSubnet(host: number, local: number, prefixLength: number): boolean {
    if (prefixLength <= 0) {
      return true;
    }
    if (prefixLength > 32) {
      return false;
    }
    const mask: number = prefixLength === 32
      ? 0xFFFFFFFF : ((0xFFFFFFFF << (32 - prefixLength)) >>> 0);
    return ((host & mask) >>> 0) === ((local & mask) >>> 0);
  }

  private static close(udp: socket.UDPSocket): void {
    udp.close().catch((error: BusinessError): void => {
      hilog.warn(DOMAIN, TAG, 'close failed: %{public}s', JSON.stringify(error));
    });
  }
}
