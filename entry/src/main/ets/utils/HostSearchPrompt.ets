import { ComponentContent, UIContext } from '@kit.ArkUI';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { MdnsHost } from '../services/MdnsDiscovery';
import { PromptDialog } from '../components/PromptDialog';

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
          .fontSize(16)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.text_primary'))
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
          .width('100%')
        Text(host.address)
          .fontSize(12)
          .fontColor($r('app.color.text_tertiary'))
          .margin({ top: 2 })
          .width('100%')
      }
      .layoutWeight(1)
      .alignItems(HorizontalAlign.Start)

      if (this.isAdded(host)) {
        Text('已添加')
          .fontSize(10)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.text_tertiary'))
          .backgroundColor($r('app.color.surface_sunken'))
          .borderRadius(4)
          .padding({ left: 6, right: 6, top: 2, bottom: 2 })
          .margin({ left: 8 })
      }
    }
    .width('100%')
    .padding({ left: 12, right: 12, top: 10, bottom: 10 })
    .borderRadius(12)
    .backgroundColor($r('app.color.control_bg'))
    .border({ width: 1, color: $r('app.color.control_border') })
    .opacity(this.isAdded(host) ? 0.5 : 1)
    .hoverEffect(this.isAdded(host) ? HoverEffect.None : HoverEffect.Highlight)
    .onClick(() => {
      if (!this.isAdded(host)) {
        this.params.onPick(host);
      }
    })
  }

  build() {
    PromptDialog({
      title: this.params.title,
      cancelLabel: '取消',
      onCancel: () => {
        this.params.onCancel();
      }
    }) {
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
    }
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
      let contentNode: ComponentContent<HostPickerParams> | undefined = undefined;

      const disposeContent = (): void => {
        const node: ComponentContent<HostPickerParams> | undefined = contentNode;
        if (node === undefined) {
          return;
        }
        try {
          node.dispose();
        } catch (error) {
          hilog.warn(DOMAIN, TAG, 'dispose host picker failed: %{public}s', JSON.stringify(error));
        }
      };
      const dismiss = (): void => {
        if (closed) {
          return;
        }
        closed = true;
        const node: ComponentContent<HostPickerParams> | undefined = contentNode;
        if (node === undefined) {
          return;
        }
        ui.getPromptAction().closeCustomDialog(node)
          .then(() => {
            disposeContent();
          })
          .catch((error: BusinessError) => {
            hilog.error(DOMAIN, TAG, 'close host picker failed: %{public}s', JSON.stringify(error));
            disposeContent();
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
      // Wire callbacks before creating the content node: the builder may
      // capture the parameter object at construction time.
      params.onPick = (host: MdnsHost): void => {
        finish(host);
      };
      params.onCancel = (): void => {
        finish(undefined);
      };
      contentNode = new ComponentContent(ui, wrapBuilder(hostPickerContent), params);

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


