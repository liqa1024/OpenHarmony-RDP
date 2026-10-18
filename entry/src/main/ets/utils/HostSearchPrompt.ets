import { ComponentContent, UIContext } from '@kit.ArkUI';
import { MdnsHost } from '../services/MdnsDiscovery';
import { PromptDialog } from '../components/PromptDialog';
import { DialogSession } from './DialogSession';
import { feedbackHost, ModalRequest } from './FeedbackHost';

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

class HostPickerRequest implements ModalRequest {
  private ui: UIContext;
  private params: HostPickerParams = new HostPickerParams();
  private session: DialogSession<HostPickerParams>;
  private resolve: (value: MdnsHost | undefined) => void;
  private done: () => void = (): void => {};
  private onClosed: () => void = (): void => {};
  private settled: boolean = false;
  private finished: boolean = false;

  constructor(ui: UIContext, hosts: MdnsHost[], title: string, addedAddresses: string[],
    resolve: (value: MdnsHost | undefined) => void) {
    this.ui = ui;
    this.params.hosts = hosts;
    this.params.title = title;
    this.params.addedAddresses = addedAddresses;
    this.resolve = resolve;
    this.session = new DialogSession<HostPickerParams>(ui);
  }

  start(done: () => void, closed: () => void): void {
    this.done = done;
    this.onClosed = closed;
    if (this.settled) {
      this.finishHost();
      return;
    }
    // Wire callbacks before creating the content node: the builder may capture
    // the parameter object at construction time.
    this.params.onPick = (host: MdnsHost): void => {
      this.finish(host);
    };
    this.params.onCancel = (): void => {
      this.finish(undefined);
    };
    const content: ComponentContent<HostPickerParams> =
      new ComponentContent(this.ui, wrapBuilder(hostPickerContent), this.params);
    this.session.open(content, (): void => {
      this.finish(undefined);
    });
  }

  abort(): void {
    if (this.settled) {
      return;
    }
    this.settled = true;
    this.session.dispose();
    this.resolve(undefined);
  }

  private finish(host: MdnsHost | undefined): void {
    if (this.settled) {
      return;
    }
    this.settled = true;
    this.session.close((): void => {
      this.onClosed();
      this.resolve(host);
      this.finishHost();
    });
  }

  private finishHost(): void {
    if (this.finished) {
      return;
    }
    this.finished = true;
    this.done();
  }
}

export function showHostPicker(ui: UIContext, hosts: MdnsHost[], title: string,
  addedAddresses: string[]): Promise<MdnsHost | undefined> {
  return new Promise<MdnsHost | undefined>(
    (resolve: (value: MdnsHost | undefined) => void) => {
      feedbackHost(ui).enqueue(new HostPickerRequest(ui, hosts, title, addedAddresses, resolve));
    });
}
