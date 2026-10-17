import { ComponentContent, UIContext } from '@kit.ArkUI';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { MdnsHost } from '../services/MdnsDiscovery';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpHostPicker';

class HostPickerParams {
  hosts: MdnsHost[] = [];
  addedAddresses: string[] = [];
  title: string = '选择主机';
  onPick: (host: MdnsHost) => void = (): void => {};
  onCancel: () => void = (): void => {};
}

@Component
struct HostPickerContent {
  params: HostPickerParams = new HostPickerParams();

  private isAdded(host: MdnsHost): boolean {
    let address: string = host.address.trim().toLowerCase();
    while (address.endsWith('.')) {
      address = address.substring(0, address.length - 1);
    }
    return this.params.addedAddresses.indexOf(address) >= 0;
  }

  @Builder
  hostRow(host: MdnsHost) {
    Row() {
      Column() {
        Text(host.name.length > 0 ? host.name : host.address)
          .fontSize(15)
          
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
          .width('100%')
        Text(host.address)
          .fontSize(12)
          
          .margin({ top: 2 })
          .width('100%')
      }
      .layoutWeight(1)
      .alignItems(HorizontalAlign.Start)

      if (this.isAdded(host)) {
        Text('已添加')
          .fontSize(11)
          
          
          .borderRadius(4)
          .padding({ left: 6, right: 6, top: 2, bottom: 2 })
          .margin({ left: 8 })
      }
    }
    .width('100%')
    .padding({ left: 12, right: 12, top: 10, bottom: 10 })
    .borderRadius(10)
    
    .opacity(this.isAdded(host) ? 0.5 : 1)
    .hoverEffect(this.isAdded(host) ? HoverEffect.None : HoverEffect.Highlight)
    .onClick(() => {
      if (!this.isAdded(host)) {
        this.params.onPick(host);
      }
    })
  }

  build() {
    Column() {
      Text(this.params.title)
        .fontSize(18)
        .fontWeight(FontWeight.Medium)
        

      Scroll() {
        Column({ space: 8 }) {
          ForEach(this.params.hosts, (host: MdnsHost) => {
            this.hostRow(host)
          }, (host: MdnsHost) => host.address)
        }
        .width('100%')
      }
      .width('100%')
      .constraintSize({ maxHeight: 280 })
      .margin({ top: 12 })
      .scrollBar(BarState.Auto)

      Button('取消')
        .width('100%')
        .height(44)
        .fontSize(16)
        
        
        .borderRadius(12)
        .margin({ top: 20 })
        .onClick(() => {
          this.params.onCancel();
        })
    }
    .width(320)
    .padding(24)
    .alignItems(HorizontalAlign.Center)
    
    .borderRadius(20)
  }
}

@Builder
function hostPickerContent(params: HostPickerParams) {
  HostPickerContent({ params: params })
}

export async function showHostPicker(ui: UIContext, hosts: MdnsHost[], title: string,
  addedAddresses: string[]): Promise<MdnsHost | undefined> {
  return new Promise<MdnsHost | undefined>(
    (resolve: (value: MdnsHost | undefined) => void) => {
      let settled: boolean = false;
      let closed: boolean = false;
      const params: HostPickerParams = new HostPickerParams();
      params.hosts = hosts;
      params.title = title;
      params.addedAddresses = addedAddresses;
      const contentNode: ComponentContent<HostPickerParams> =
        new ComponentContent(ui, wrapBuilder(hostPickerContent), params);

      const dismiss = (): void => {
        if (closed) {
          return;
        }
        closed = true;
        ui.getPromptAction().closeCustomDialog(contentNode)
          .then(() => {
            contentNode.dispose();
          })
          .catch((error: BusinessError) => {
            hilog.error(DOMAIN, TAG, 'close host picker failed: %{public}s', JSON.stringify(error));
          });
      };
      const finish = (host: MdnsHost | undefined): void => {
        if (settled) {
          return;
        }
        settled = true;
        dismiss();
        resolve(host);
      };
      params.onPick = (host: MdnsHost): void => {
        finish(host);
      };
      params.onCancel = (): void => {
        finish(undefined);
      };

      ui.getPromptAction().openCustomDialog(contentNode,
        { isModal: true, autoCancel: false })
        .then(() => {
          if (settled) {
            dismiss();
          }
        })
        .catch((error: BusinessError) => {
          hilog.error(DOMAIN, TAG, 'open host picker failed: %{public}s', JSON.stringify(error));
          finish(undefined);
        });
    });
}


