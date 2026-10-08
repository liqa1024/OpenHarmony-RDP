/*
 * HmRdp - host picker dialog.
 *
 * A modal list of already-discovered hosts (see services/MdnsDiscovery). The
 * caller runs the search and only opens this once it has candidates, so the
 * dialog never carries a loading state - progress lives on the trigger control
 * itself (see ui-design.md §4.2). Picking a row returns that host; cancelling
 * returns `undefined`.
 */
import { ComponentContent, UIContext } from '@kit.ArkUI';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { MdnsHost } from '../services/MdnsDiscovery';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpHostPicker';

class HostPickerParams {
  hosts: MdnsHost[] = [];
  title: string = '选择主机';
  onPick: (host: MdnsHost) => void = (): void => {};
  onCancel: () => void = (): void => {};
}

@Component
struct HostPickerContent {
  params: HostPickerParams = new HostPickerParams();

  @Builder
  hostRow(host: MdnsHost) {
    Row() {
      Column() {
        Text(host.name.length > 0 ? host.name : host.address)
          .fontSize(15)
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
    }
    .width('100%')
    .padding({ left: 12, right: 12, top: 10, bottom: 10 })
    .borderRadius(10)
    .backgroundColor($r('app.color.input_bg'))
    .hoverEffect(HoverEffect.Highlight)
    .onClick(() => {
      this.params.onPick(host);
    })
  }

  build() {
    Column() {
      Text(this.params.title)
        .fontSize(18)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary'))

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
        .fontColor($r('app.color.text_secondary'))
        .backgroundColor($r('app.color.input_bg'))
        .borderRadius(12)
        .margin({ top: 20 })
        .onClick(() => {
          this.params.onCancel();
        })
    }
    .width(320)
    .padding(24)
    .alignItems(HorizontalAlign.Center)
    .backgroundColor($r('app.color.card_bg'))
    .borderRadius(20)
  }
}

@Builder
function hostPickerContent(params: HostPickerParams) {
  HostPickerContent({ params: params })
}

/**
 * Shows `hosts` for the user to pick; resolves the picked host or `undefined`
 * on cancel. `title` labels the dialog.
 */
export async function showHostPicker(ui: UIContext, hosts: MdnsHost[],
  title: string): Promise<MdnsHost | undefined> {
  return new Promise<MdnsHost | undefined>(
    (resolve: (value: MdnsHost | undefined) => void) => {
      let settled: boolean = false;
      let closed: boolean = false;
      const params: HostPickerParams = new HostPickerParams();
      params.hosts = hosts;
      params.title = title;
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

      ui.getPromptAction().openCustomDialog(contentNode, { isModal: true, autoCancel: false })
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
