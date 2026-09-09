/*
 * HmRdp - add / edit a saved connection.
 */
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { util } from '@kit.ArkTS';
import { DisplayProfile, EditConnectionParams, PendingConnection, RdpConnectOptions,
  RdpConnectOrigin, SavedConnection } from '../model/RdpModels';
import { ConnectionStore } from '../services/ConnectionStore';
import { CredentialStore } from '../services/CredentialStore';
import { SessionManager } from '../services/SessionManager';
import { SCALE_PRESETS, SettingsStore } from '../services/SettingsStore';

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
  @State scaleText: string = '100';
  @State useGlobalDisplay: boolean = true;
  @State advancedExpanded: boolean = false;
  @State enableClipboard: boolean = true;
  @State enableAudio: boolean = true;
  @State enableGfx: boolean = true;
  @State enableH264: boolean = true;
  @State ignoreCertificate: boolean = true;
  @State errorMessage: string = '';
  @State editing: boolean = false;
  @State detectedText: string = '';
  @State recommendedScale: number = 100;
  private connectionId: string = '';
  private storedPassword: string = '';
  private returned: boolean = false;

  aboutToAppear(): void {
    const detected: DisplayProfile = SettingsStore.detectedDisplay();
    this.detectedText = detected.valid()
      ? `${detected.width} × ${detected.height}` : '无法获取显示器分辨率';
    this.recommendedScale = SettingsStore.recommendedScalePercent();
    this.desktopWidthText = detected.valid() ? detected.width.toString() : '1920';
    this.desktopHeightText = detected.valid() ? detected.height.toString() : '1080';
    this.scaleText = this.recommendedScale.toString();

    const params: EditConnectionParams =
      this.getUIContext().getRouter().getParams() as EditConnectionParams;
    const id: string = params?.id ?? '';
    this.connectionId = id;
    this.editing = id.length > 0;
    if (!this.editing) {
      return;
    }
    const conn: SavedConnection | undefined = ConnectionStore.get(id);
    if (conn !== undefined) {
      this.host = conn.host;
      this.port = conn.port.toString();
      this.username = conn.username;
      this.domain = conn.domain;
      this.useGlobalDisplay = conn.useGlobalDisplay;
      if (conn.width > 0 && conn.height > 0) {
        this.desktopWidthText = conn.width.toString();
        this.desktopHeightText = conn.height.toString();
      }
      if (conn.scalePercent > 0) {
        this.scaleText = conn.scalePercent.toString();
      }
      this.enableClipboard = conn.enableClipboard;
      this.enableAudio = conn.enableAudio;
      this.enableGfx = conn.enableGfx;
      this.enableH264 = conn.enableH264;
      this.ignoreCertificate = conn.ignoreCertificate;
    }
    CredentialStore.loadPassword(id)
      .then((password: string) => {
        this.storedPassword = password;
        this.password = password;
      })
      .catch((error: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'load password failed: %{public}s', JSON.stringify(error));
      });
  }

  private parsePort(): number {
    const value: number = Number.parseInt(this.port, 10);
    return Number.isNaN(value) || value <= 0 ? 3389 : value;
  }

  private parseSize(text: string, fallback: number): number {
    const value: number = Number.parseInt(text, 10);
    return Number.isNaN(value) || value <= 0 ? fallback : value;
  }

  private parseScale(text: string, fallback: number): number {
    const value: number = Number.parseInt(text, 10);
    if (Number.isNaN(value) || value < 100) {
      return fallback;
    }
    return Math.min(value, 500);
  }

  private parsedScale(): number {
    const value: number = Number.parseInt(this.scaleText, 10);
    return Number.isNaN(value) ? 0 : value;
  }

  private isPresetScale(value: number): boolean {
    return SCALE_PRESETS.indexOf(value) >= 0;
  }

  private buildOptions(): RdpConnectOptions | undefined {
    const target: string = this.host.trim();
    if (target.length === 0) {
      this.errorMessage = '请输入主机名或 IP 地址';
      return undefined;
    }
    this.errorMessage = '';
    const global: DisplayProfile = SettingsStore.globalDisplayProfile();
    const options: RdpConnectOptions =
      RdpConnectOptions.of(target, this.parsePort(), this.username.trim(), this.password,
        this.domain.trim());
    options.width = this.useGlobalDisplay
      ? global.width : this.parseSize(this.desktopWidthText, global.width);
    options.height = this.useGlobalDisplay
      ? global.height : this.parseSize(this.desktopHeightText, global.height);
    options.scalePercent = this.useGlobalDisplay
      ? global.scalePercent : this.parseScale(this.scaleText, global.scalePercent);
    options.enableClipboard = this.enableClipboard;
    options.enableAudio = this.enableAudio;
    options.enableGfx = this.enableGfx;
    options.enableH264 = this.enableH264;
    options.ignoreCertificate = this.ignoreCertificate;
    return options;
  }

  private submit(connect: boolean): void {
    const options: RdpConnectOptions | undefined = this.buildOptions();
    if (options === undefined) {
      return;
    }
    const id: string = this.connectionId.length > 0
      ? this.connectionId : util.generateRandomUUID(true);
    const conn: SavedConnection = SavedConnection.fromOptions(id, options);
    conn.useGlobalDisplay = this.useGlobalDisplay;
    conn.updatedAt = Date.now();
    try {
      ConnectionStore.save(conn);
    } catch (error) {
      this.errorMessage = '保存失败，请重试';
      hilog.error(DOMAIN, TAG, 'save failed: %{public}s', JSON.stringify(error));
      return;
    }
    if (connect) {
      options.password = this.password;
      const request: PendingConnection = new PendingConnection();
      request.options = options;
      request.connectionId = id;
      request.origin = RdpConnectOrigin.Editor;
      request.onConnected = undefined;
      const context = this.getUIContext().getHostContext();
      if (context === undefined) {
        this.errorMessage = '无法发起连接';
        return;
      }
      SessionManager.connect(request, context as common.UIAbilityContext);
    }
    // Return to the list so the connection progress / result is visible there.
    this.returnToList();
  }

  private handleSave(): void {
    if (this.password.length > 0 && this.password !== this.storedPassword) {
      this.getUIContext().showAlertDialog({
        title: '密码不会被保存',
        message: '连接验证正确性后会自动保存密码。',
        primaryButton: {
          value: '取消',
          action: () => {
          }
        },
        secondaryButton: {
          value: '继续保存',
          fontColor: $r('app.color.accent'),
          action: () => {
            this.submit(false);
          }
        }
      });
      return;
    }
    this.submit(false);
  }

  private returnToList(): void {
    if (this.returned) {
      return;
    }
    this.returned = true;
    this.getUIContext().getRouter().back();
  }

  private deleteCurrent(): void {
    if (this.connectionId.length > 0) {
      ConnectionStore.remove(this.connectionId);
      CredentialStore.removePassword(this.connectionId).catch((error: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'remove password failed: %{public}s', JSON.stringify(error));
      });
    }
    this.returnToList();
  }

  @Builder
  fieldLabel(text: string) {
    Text(text)
      .fontSize(12)
      .fontColor($r('app.color.text_secondary'))
      .margin({ bottom: 6 })
  }

  @Builder
  scaleChip(preset: number) {
    Text(preset.toString() + '%')
      .fontSize(13)
      .fontColor(this.parsedScale() === preset ? '#FFFFFF' : $r('app.color.text_secondary'))
      .backgroundColor(this.parsedScale() === preset
        ? $r('app.color.accent') : $r('app.color.input_bg'))
      .borderRadius(8)
      .padding({ left: 14, right: 14, top: 6, bottom: 6 })
      .margin({ right: 8, bottom: 8 })
      .onClick(() => {
        this.scaleText = preset.toString();
      })
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
                  this.handleSave();
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
                Text('连接成功后自动保存')
                  .fontSize(11)
                  .fontColor($r('app.color.text_tertiary'))
                  .margin({ top: 4 })
              }
              .layoutWeight(1)
              .margin({ left: 16 })
              .alignItems(HorizontalAlign.Start)
            }
            .width('100%')
            .margin({ bottom: 16 })

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

            Divider()
              .color($r('app.color.divider'))
              .margin({ top: 20, bottom: 8 })

            Row() {
              Column() {
                Text('高级设置').fontSize(14).fontColor($r('app.color.text_primary'))
                Text('显示、剪贴板、音频等连接独立选项')
                  .fontSize(11)
                  .fontColor($r('app.color.text_tertiary'))
                  .margin({ top: 2 })
              }
              .alignItems(HorizontalAlign.Start)
              .layoutWeight(1)

              Row() {
                if (!this.useGlobalDisplay) {
                  Text('已自定义')
                    .fontSize(11)
                    .fontColor($r('app.color.text_tertiary'))
                    .margin({ right: 8 })
                }
                Text(this.advancedExpanded ? '收起' : '展开')
                  .fontSize(13)
                  .fontColor($r('app.color.accent'))
              }
              .alignItems(VerticalAlign.Center)
            }
            .width('100%')
            .padding({ top: 8, bottom: 8 })
            .onClick(() => {
              this.advancedExpanded = !this.advancedExpanded;
            })

            if (this.advancedExpanded) {
              Column() {
                Row() {
                  Column() {
                    Text('使用全局显示设置').fontSize(14).fontColor($r('app.color.text_primary'))
                    Text('分辨率与缩放跟随全局设置')
                      .fontSize(11)
                      .fontColor($r('app.color.text_tertiary'))
                      .margin({ top: 2 })
                  }
                  .alignItems(HorizontalAlign.Start)
                  .layoutWeight(1)

                  Toggle({ type: ToggleType.Switch, isOn: this.useGlobalDisplay })
                    .selectedColor($r('app.color.accent'))
                    .onChange((isOn: boolean) => {
                      this.useGlobalDisplay = isOn;
                    })
                }
                .width('100%')
                .height(58)

                if (this.useGlobalDisplay) {
                  Text('自动检测：' + this.detectedText + ' · 推荐缩放 '
                    + this.recommendedScale + '%')
                    .fontSize(11)
                    .fontColor($r('app.color.text_tertiary'))
                    .width('100%')
                } else {
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
                  .margin({ top: 12 })

                  Text('自动检测：' + this.detectedText)
                    .fontSize(11)
                    .fontColor($r('app.color.text_tertiary'))
                    .width('100%')
                    .margin({ top: 6 })

                  Column() {
                    this.fieldLabel('缩放比例')
                    Flex({ wrap: FlexWrap.Wrap }) {
                      ForEach(SCALE_PRESETS, (preset: number) => {
                        this.scaleChip(preset)
                      }, (preset: number) => preset.toString())
                    }
                    .width('100%')
                    .margin({ top: 6 })

                    TextInput({ placeholder: '100', text: this.scaleText })
                      .width('100%')
                      .height(46)
                      .borderRadius(10)
                      .backgroundColor($r('app.color.input_bg'))
                      .type(InputType.Number)
                      .onChange((value: string) => {
                        this.scaleText = value;
                      })

                    if (this.parsedScale() > 0 && !this.isPresetScale(this.parsedScale())) {
                      Text('自定义缩放比例可能导致部分应用显示模糊，建议使用上方固定档位')
                        .fontSize(11)
                        .fontColor($r('app.color.danger'))
                        .margin({ top: 6 })
                    } else {
                      Text('推荐 ' + this.recommendedScale + '%')
                        .fontSize(11)
                        .fontColor($r('app.color.text_tertiary'))
                        .margin({ top: 4 })
                    }
                  }
                  .width('100%')
                  .margin({ top: 16 })
                  .alignItems(HorizontalAlign.Start)
                }

                Divider()
                  .color($r('app.color.divider'))
                  .margin({ top: 8, bottom: 4 })

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
              }
              .width('100%')
              .alignItems(HorizontalAlign.Start)
            }
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
