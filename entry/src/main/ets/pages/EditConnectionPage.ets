/*
 * HmRdp - add / edit a saved connection.
 */
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { EditConnectionParams, PendingConnection, RdpConnectOptions,
  SavedConnection } from '../model/RdpModels';
import { CredentialStore } from '../services/CredentialStore';
import { WindowController } from '../services/WindowController';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpEdit';

@Entry
@Component
struct EditConnectionPage {
  @State host: string = '';
  @State port: string = '3389';
  @State username: string = '';
  @State password: string = '';
  @State domain: string = '';
  @State desktopWidthText: string = '1920';
  @State desktopHeightText: string = '1080';
  @State enableClipboard: boolean = true;
  @State enableAudio: boolean = true;
  @State enableGfx: boolean = true;
  @State enableH264: boolean = true;
  @State ignoreCertificate: boolean = true;
  @State errorMessage: string = '';
  @State editing: boolean = false;
  private alias: string = '';

  aboutToAppear(): void {
    const params: EditConnectionParams =
      this.getUIContext().getRouter().getParams() as EditConnectionParams;
    const alias: string = params?.alias ?? '';
    this.alias = alias;
    this.editing = alias.length > 0;
    if (this.editing) {
      CredentialStore.loadConnection(alias)
        .then((conn: SavedConnection | undefined) => {
          if (conn !== undefined) {
            this.host = conn.host;
            this.port = conn.port.toString();
            this.username = conn.username;
            this.password = conn.password;
            this.domain = conn.domain;
            this.desktopWidthText = conn.width.toString();
            this.desktopHeightText = conn.height.toString();
            this.enableClipboard = conn.enableClipboard;
            this.enableAudio = conn.enableAudio;
            this.enableGfx = conn.enableGfx;
            this.enableH264 = conn.enableH264;
            this.ignoreCertificate = conn.ignoreCertificate;
          }
        })
        .catch((error: BusinessError) => {
          hilog.error(DOMAIN, TAG, 'load failed: %{public}s', JSON.stringify(error));
        });
    }
  }

  private parsePort(): number {
    const value: number = Number.parseInt(this.port, 10);
    return Number.isNaN(value) || value <= 0 ? 3389 : value;
  }

  private parseSize(text: string, fallback: number): number {
    const value: number = Number.parseInt(text, 10);
    return Number.isNaN(value) || value <= 0 ? fallback : value;
  }

  private buildOptions(): RdpConnectOptions | undefined {
    const target: string = this.host.trim();
    if (target.length === 0) {
      this.errorMessage = '请输入主机名或 IP 地址';
      return undefined;
    }
    this.errorMessage = '';
    const options: RdpConnectOptions =
      RdpConnectOptions.of(target, this.parsePort(), this.username.trim(), this.password,
        this.domain.trim());
    options.width = this.parseSize(this.desktopWidthText, 1920);
    options.height = this.parseSize(this.desktopHeightText, 1080);
    options.enableClipboard = this.enableClipboard;
    options.enableAudio = this.enableAudio;
    options.enableGfx = this.enableGfx;
    options.enableH264 = this.enableH264;
    options.ignoreCertificate = this.ignoreCertificate;
    return options;
  }

  private async submit(connect: boolean): Promise<void> {
    const options: RdpConnectOptions | undefined = this.buildOptions();
    if (options === undefined) {
      return;
    }
    const newAlias: string = CredentialStore.aliasOf(options.host, options.port);
    if (this.editing && this.alias.length > 0 && this.alias !== newAlias) {
      await CredentialStore.remove(this.alias);
    }
    const conn: SavedConnection = SavedConnection.fromOptions(newAlias, options);
    try {
      await CredentialStore.saveConnection(conn);
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'save failed: %{public}s', JSON.stringify(error));
    }
    if (connect) {
      PendingConnection.current.options = conn.toOptions();
      PendingConnection.current.remember = true;
      const context = this.getUIContext().getHostContext();
      if (context !== undefined) {
        WindowController.openSessionWindow(context as common.UIAbilityContext);
      } else {
        this.errorMessage = '无法打开会话窗口';
      }
    } else {
      this.getUIContext().getRouter().back();
    }
  }

  private async deleteCurrent(): Promise<void> {
    if (this.alias.length > 0) {
      await CredentialStore.remove(this.alias);
    }
    this.getUIContext().getRouter().back();
  }

  @Builder
  fieldLabel(text: string) {
    Text(text)
      .fontSize(12)
      .fontColor($r('app.color.text_secondary'))
      .margin({ bottom: 6 })
  }

  @Builder
  optionRow(title: string, subtitle: string, value: boolean,
    onToggle: (v: boolean) => void) {
    Row() {
      Column() {
        Text(title).fontSize(14).fontColor($r('app.color.text_primary'))
        if (subtitle.length > 0) {
          Text(subtitle).fontSize(11).fontColor($r('app.color.text_tertiary'))
            .margin({ top: 2 })
        }
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)

      Toggle({ type: ToggleType.Switch, isOn: value })
        .onChange((isOn: boolean) => {
          onToggle(isOn);
        })
    }
    .width('100%')
    .height(subtitle.length > 0 ? 58 : 48)
  }

  build() {
    Column() {
      Scroll() {
        Column() {
          Column() {
            Row() {
              Text('取消')
                .fontSize(14)
                .fontColor($r('app.color.accent'))
                .padding({ top: 8, bottom: 8 })
                .onClick(() => {
                  this.getUIContext().getRouter().back();
                })
              Text(this.editing ? '编辑连接' : '新增连接')
                .fontSize(17)
                .fontWeight(FontWeight.Medium)
                .fontColor($r('app.color.text_primary'))
                .layoutWeight(1)
                .textAlign(TextAlign.Center)
              Text('保存')
                .fontSize(14)
                .fontColor($r('app.color.accent'))
                .padding({ top: 8, bottom: 8 })
                .onClick(() => {
                  this.submit(false);
                })
            }
            .width('100%')

            Divider()
              .color($r('app.color.divider'))
              .margin({ top: 12, bottom: 20 })

            this.fieldLabel('主机名或 IP 地址')
            TextInput({ placeholder: '例如 192.168.1.100', text: this.host })
              .width('100%')
              .height(46)
              .borderRadius(10)
              .backgroundColor($r('app.color.input_bg'))
              .margin({ bottom: 16 })
              .onChange((value: string) => {
                this.host = value;
              })

            Row() {
              Column() {
                this.fieldLabel('端口')
                TextInput({ placeholder: '3389', text: this.port })
                  .width('100%')
                  .height(46)
                  .borderRadius(10)
                  .backgroundColor($r('app.color.input_bg'))
                  .type(InputType.Number)
                  .onChange((value: string) => {
                    this.port = value;
                  })
              }
              .layoutWeight(1)
              .alignItems(HorizontalAlign.Start)

              Column() {
                this.fieldLabel('域（可选）')
                TextInput({ placeholder: 'WORKGROUP', text: this.domain })
                  .width('100%')
                  .height(46)
                  .borderRadius(10)
                  .backgroundColor($r('app.color.input_bg'))
                  .onChange((value: string) => {
                    this.domain = value;
                  })
              }
              .layoutWeight(1)
              .margin({ left: 16 })
              .alignItems(HorizontalAlign.Start)
            }
            .width('100%')
            .margin({ bottom: 16 })

            Row() {
              Column() {
                this.fieldLabel('用户名')
                TextInput({ placeholder: '用户名', text: this.username })
                  .width('100%')
                  .height(46)
                  .borderRadius(10)
                  .backgroundColor($r('app.color.input_bg'))
                  .onChange((value: string) => {
                    this.username = value;
                  })
              }
              .layoutWeight(1)
              .alignItems(HorizontalAlign.Start)

              Column() {
                this.fieldLabel('密码')
                TextInput({ placeholder: '密码', text: this.password })
                  .width('100%')
                  .height(46)
                  .borderRadius(10)
                  .backgroundColor($r('app.color.input_bg'))
                  .type(InputType.Password)
                  .onChange((value: string) => {
                    this.password = value;
                  })
              }
              .layoutWeight(1)
              .margin({ left: 16 })
              .alignItems(HorizontalAlign.Start)
            }
            .width('100%')
            .margin({ bottom: 16 })

            Row() {
              Column() {
                this.fieldLabel('分辨率宽度')
                TextInput({ placeholder: '1920', text: this.desktopWidthText })
                  .width('100%')
                  .height(46)
                  .borderRadius(10)
                  .backgroundColor($r('app.color.input_bg'))
                  .type(InputType.Number)
                  .onChange((value: string) => {
                    this.desktopWidthText = value;
                  })
              }
              .layoutWeight(1)
              .alignItems(HorizontalAlign.Start)

              Column() {
                this.fieldLabel('分辨率高度')
                TextInput({ placeholder: '1080', text: this.desktopHeightText })
                  .width('100%')
                  .height(46)
                  .borderRadius(10)
                  .backgroundColor($r('app.color.input_bg'))
                  .type(InputType.Number)
                  .onChange((value: string) => {
                    this.desktopHeightText = value;
                  })
              }
              .layoutWeight(1)
              .margin({ left: 16 })
              .alignItems(HorizontalAlign.Start)
            }
            .width('100%')

            Divider()
              .color($r('app.color.divider'))
              .margin({ top: 20, bottom: 8 })

            this.optionRow('剪贴板重定向', '', this.enableClipboard, (v: boolean) => {
              this.enableClipboard = v;
            })
            this.optionRow('音频重定向', '', this.enableAudio, (v: boolean) => {
              this.enableAudio = v;
            })
            this.optionRow('GFX 图形管道', '更流畅的画面传输', this.enableGfx, (v: boolean) => {
              this.enableGfx = v;
            })
            this.optionRow('H.264 / AVC 解码', '需服务端支持', this.enableH264, (v: boolean) => {
              this.enableH264 = v;
            })
            this.optionRow('忽略证书校验', '仅用于自签名证书的测试环境', this.ignoreCertificate,
              (v: boolean) => {
                this.ignoreCertificate = v;
              })

            if (this.errorMessage.length > 0) {
              Text(this.errorMessage)
                .fontSize(13)
                .fontColor($r('app.color.danger'))
                .width('100%')
                .margin({ top: 12 })
            }

            Row() {
              if (this.editing) {
                Button('删除')
                  .layoutWeight(1)
                  .height(48)
                  .fontSize(16)
                  .fontColor($r('app.color.danger'))
                  .backgroundColor($r('app.color.input_bg'))
                  .borderRadius(12)
                  .onClick(() => {
                    this.deleteCurrent();
                  })
                Button('连接')
                  .layoutWeight(2)
                  .height(48)
                  .fontSize(16)
                  .fontWeight(FontWeight.Medium)
                  .backgroundColor($r('app.color.accent'))
                  .borderRadius(12)
                  .margin({ left: 12 })
                  .onClick(() => {
                    this.submit(true);
                  })
              } else {
                Button('连接')
                  .width('100%')
                  .height(48)
                  .fontSize(16)
                  .fontWeight(FontWeight.Medium)
                  .backgroundColor($r('app.color.accent'))
                  .borderRadius(12)
                  .onClick(() => {
                    this.submit(true);
                  })
              }
            }
            .width('100%')
            .margin({ top: 20 })
          }
          .width('100%')
          .constraintSize({ maxWidth: 720 })
          .padding(28)
          .backgroundColor($r('app.color.card_bg'))
          .borderRadius(20)
          .alignItems(HorizontalAlign.Start)
          .shadow({ radius: 24, color: '#14000000', offsetX: 0, offsetY: 8 })
        }
        .width('100%')
        .padding({ top: 32, bottom: 32, left: 24, right: 24 })
        .alignItems(HorizontalAlign.Center)
      }
      .layoutWeight(1)
      .scrollBar(BarState.Auto)
      .align(Alignment.Top)
    }
    .width('100%')
    .height('100%')
    .backgroundColor($r('app.color.page_bg'))
  }
}
