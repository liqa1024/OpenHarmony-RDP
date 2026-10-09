import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { mdns } from '@kit.NetworkKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpMdns';

export class MdnsHost {
  name: string = '';
  address: string = '';
  port: number = 0;
  // `serviceLost` carries only the raw instance name (no address), so a lost
  // instance is matched against it.
  serviceKey: string = '';
}

export class MdnsDiscovery {
  private context: common.UIAbilityContext | undefined = undefined;
  private service: mdns.DiscoveryService | undefined = undefined;
  private hosts: MdnsHost[] = [];
  private onUpdate: (hosts: MdnsHost[]) => void = (): void => {};

  start(context: common.UIAbilityContext, serviceType: string,
    onUpdate: (hosts: MdnsHost[]) => void): void {
    this.stop();
    this.context = context;
    this.onUpdate = onUpdate;
    try {
      this.service = mdns.createDiscoveryService(context, serviceType);
      this.service.on('serviceFound', (data: mdns.LocalServiceInfo): void => {
        this.resolve(data);
      });
      this.service.on('serviceLost', (data: mdns.LocalServiceInfo): void => {
        this.remove(data.serviceName);
      });
      this.service.startSearchingMDNS();
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'start failed: %{public}s', JSON.stringify(error));
    }
  }

  stop(): void {
    const service: mdns.DiscoveryService | undefined = this.service;
    this.service = undefined;
    this.context = undefined;
    if (service !== undefined) {
      try {
        service.stopSearchingMDNS();
        service.off('serviceFound');
        service.off('serviceLost');
      } catch (error) {
        hilog.warn(DOMAIN, TAG, 'stop failed: %{public}s', JSON.stringify(error));
      }
    }
  }

  private resolve(data: mdns.LocalServiceInfo): void {
    const context: common.UIAbilityContext | undefined = this.context;
    if (context === undefined) {
      return;
    }
    mdns.resolveLocalService(context, data)
      .then((resolved: mdns.LocalServiceInfo): void => {
        this.upsert(resolved);
      })
      .catch((error: BusinessError): void => {
        hilog.warn(DOMAIN, TAG, 'resolve %{public}s failed: %{public}s',
          data.serviceName, JSON.stringify(error));
      });
  }

  private upsert(info: mdns.LocalServiceInfo): void {
    const address: string = info.host !== undefined ? info.host.address : '';
    if (address.length === 0) {
      return;
    }
    const name: string = MdnsDiscovery.displayName(info.serviceName);
    const port: number = info.port !== undefined ? info.port : 0;
    const next: MdnsHost[] = [];
    let exists: boolean = false;
    let changed: boolean = false;
    for (let i = 0; i < this.hosts.length; i++) {
      const host: MdnsHost = this.hosts[i];
      if (host.address === address) {
        exists = true;
        if (host.name !== name) {
          host.name = name;
          changed = true;
        }
        if (host.serviceKey !== info.serviceName) {
          host.serviceKey = info.serviceName;
          changed = true;
        }
        if (host.port !== port) {
          host.port = port;
          changed = true;
        }
      }
      next.push(host);
    }
    if (!exists) {
      const host: MdnsHost = new MdnsHost();
      host.name = name;
      host.address = address;
      host.serviceKey = info.serviceName;
      host.port = port;
      next.push(host);
      changed = true;
    }
    if (changed) {
      next.sort((left: MdnsHost, right: MdnsHost): number => {
        return left.name < right.name ? -1 : (left.name > right.name ? 1 : 0);
      });
      this.hosts = next;
      this.onUpdate(this.hosts);
    }
  }

  private remove(serviceKey: string): void {
    const next: MdnsHost[] = [];
    let changed: boolean = false;
    for (let i = 0; i < this.hosts.length; i++) {
      if (this.hosts[i].serviceKey === serviceKey) {
        changed = true;
        continue;
      }
      next.push(this.hosts[i]);
    }
    if (changed) {
      this.hosts = next;
      this.onUpdate(this.hosts);
    }
  }

  // Instance names look like "Sunshine" or "PC._nvstream._tcp.local"; keep the first label.
  private static displayName(serviceName: string): string {
    let name: string = serviceName;
    const dot: number = name.indexOf('.');
    if (dot > 0) {
      name = name.substring(0, dot);
    }
    return name.length > 0 ? name : serviceName;
  }
}
