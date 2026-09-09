/*
 * HmRdp - saved connections list (home page).
 */
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { DisplayProfile, EditConnectionParams, PendingConnection, RdpConnectOptions,
  RdpConnectOrigin, SavedConnection, SessionRequests } from '../model/RdpModels';
import { ConnectionStore } from '../services/ConnectionStore';
import { CredentialStore } from '../services/CredentialStore';
import { SettingsStore } from '../services/SettingsStore';
import { WindowController } from '../services/WindowController';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpIndex';

@Entry
@Component
struct Index {
  @State connections: SavedConnection[] = [];
  @State settingsHovered: boolean = false;
  @State settingsPressed: boolean = false;
  @State fabHovered: boolean = false;
  @State fabPressed: boolean = false;

  aboutToAppear(): void {
    this.refresh();
  }

  onPageShow(): void {
    this.refresh();
  }

  private refresh(): void {
    this.connections = ConnectionStore.list();
  }

  private connect(conn: SavedConnection): void {
    this.startConnection(conn);
  }

  private async startConnection(conn: SavedConnection): Promise<void> {
    const options: RdpConnectOptions = conn.toOptions();
    const profile: DisplayProfile = SettingsStore.resolveDisplay(conn);
    options.width = profile.width;
    options.height = profile.height;
    options.scalePercent = profile.scalePercent;
    const password: string = await CredentialStore.loadPassword(conn.id);
    options.password = password;
    const request: PendingConnection = new PendingConnection();
    request.options = options;
    request.connectionId = conn.id;
    request.origin = RdpConnectOrigin.List;
    request.onConnected = undefined;
    const sessionKey: string = SessionRequests.newKey();
    SessionRequests.put(sessionKey, request);
    const context = this.getUIContext().getHostContext();
    if (context !== undefined) {
      WindowController.openSessionWindow(context as common.UIAbilityContext, sessionKey);
    }
  }

  private openEditor(id: string): void {
    const params: EditConnectionParams = { id: id };
    this.getUIContext().getRouter()
      .pushUrl({ url: 'pages/EditConnectionPage', params: params })
      .catch((error: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'open editor failed: %{public}s', JSON.stringify(error));
      });
  }

  private openSettings(): void {
    this.getUIContext().getRouter().pushUrl({ url: 'pages/SettingsPage' })
      .catch((error: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'open settings failed: %{public}s', JSON.stringify(error));
      });
  }

  private remove(conn: SavedConnection): void {
    ConnectionStore.remove(conn.id);
    CredentialStore.removePassword(conn.id)
      .then(() => {
        this.refresh();
      })
      .catch((error: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'remove failed: %{public}s', JSON.stringify(error));
        this.refresh();
      });
  }

  private confirmDelete(conn: SavedConnection): void {
    this.getUIContext().showAlertDialog({
      title: '删除连接',
      message: `确定要删除「${conn.label()}」？该操作无法撤销。`,
      primaryButton: {
        value: '取消',
        action: () => {
        }
      },
      secondaryButton: {
        value: '删除',
        fontColor: $r('app.color.danger'),
        action: () => {
          this.remove(conn);
        }
      }
    });
  }

  @Builder
  connectionMenu(conn: SavedConnection) {
    Menu() {
      MenuItem({ content: '连接' })
        .onClick(() => {
          this.connect(conn);
        })
      MenuItem({ content: '编辑' })
        .onClick(() => {
          this.openEditor(conn.id);
        })
      MenuItem({ content: '删除' })
        .contentFontColor($r('app.color.danger'))
        .onClick(() => {
          this.confirmDelete(conn);
        })
    }
  }

  @Builder
  connectionRow(conn: SavedConnection) {
    Row() {
      Row() {
        Image($r('app.media.app_icon'))
          .width(44)
          .height(44)
          .borderRadius(12)
        Column() {
          Text(conn.label())
            .fontSize(15)
            .fontColor($r('app.color.text_primary'))
          Text(conn.username.length > 0 ? conn.username : '未设置用户名')
            .fontSize(12)
            .fontColor($r('app.color.text_tertiary'))
            .margin({ top: 3 })
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)
        .margin({ left: 14 })
      }
      .layoutWeight(1)
      .onClick(() => {
        this.openEditor(conn.id);
      })

      Row() {
        Image($r('app.media.ic_arrow_right'))
          .width(18)
          .height(18)
      }
      .width(40)
      .height(40)
      .borderRadius(20)
      .backgroundColor($r('app.color.accent'))
      .justifyContent(FlexAlign.Center)
      .alignItems(VerticalAlign.Center)
      .margin({ left: 8 })
      .onClick(() => {
        this.connect(conn);
      })
    }
    .width('100%')
    .padding(14)
    .borderRadius(16)
    .backgroundColor($r('app.color.card_bg'))
    .shadow({ radius: 12, color: '#10000000', offsetX: 0, offsetY: 3 })
    .bindContextMenu(this.connectionMenu(conn), ResponseType.RightClick)
  }

  build() {
    Stack({ alignContent: Alignment.BottomEnd }) {
      Column() {
      Row() {
        Image($r('app.media.app_icon'))
          .width(44)
          .height(44)
          .borderRadius(11)
        Column() {
          Text('RDP 远程桌面')
            .fontSize(20)
            .fontWeight(FontWeight.Bold)
            .fontColor($r('app.color.text_primary'))
          Text('远程控制 Windows 桌面')
            .fontSize(12)
            .fontColor($r('app.color.text_secondary'))
            .margin({ top: 2 })
        }
        .alignItems(HorizontalAlign.Start)
        .margin({ left: 12 })

        Blank()

        Stack() {
          Row()
            .width(44)
            .height(44)
            .borderRadius(22)
            .backgroundColor($r('app.color.hover_bg'))
            .opacity(this.settingsHovered ? 1.0 : 0.0)
            .animation({ duration: 150, curve: Curve.FastOutSlowIn })

          Image($r('app.media.ic_settings'))
            .width(24)
            .height(24)
            .opacity(this.settingsPressed ? 0.55 : 1.0)
            .scale({
              x: this.settingsPressed ? 0.84 : 1.0,
              y: this.settingsPressed ? 0.84 : 1.0
            })
            .animation({ duration: 150, curve: Curve.FastOutSlowIn })
        }
        .width(44)
        .height(44)
        .onHover((isHover: boolean) => {
          this.settingsHovered = isHover;
        })
        .onMouse((event: MouseEvent) => {
          if (event.action === MouseAction.Press) {
            this.settingsPressed = true;
          } else if (event.action === MouseAction.Release) {
            this.settingsPressed = false;
          }
        })
        .onClick(() => {
          this.openSettings();
        })
      }
      .width('100%')
      .padding({ left: 24, right: 24, top: 20, bottom: 16 })

      Divider().color($r('app.color.divider'))

      if (this.connections.length === 0) {
        Column() {
          Image($r('app.media.app_icon'))
            .width(72)
            .height(72)
            .borderRadius(18)
            .opacity(0.35)
          Text('还没有保存的连接')
            .fontSize(15)
            .fontColor($r('app.color.text_secondary'))
            .margin({ top: 18 })
          Text('点击右上角 + 添加一个远程桌面')
            .fontSize(13)
            .fontColor($r('app.color.text_tertiary'))
            .margin({ top: 6 })
          Button('添加连接')
            .height(44)
            .fontSize(15)
            .backgroundColor($r('app.color.accent'))
            .borderRadius(12)
            .margin({ top: 22 })
            .onClick(() => {
              this.openEditor('');
            })
        }
        .layoutWeight(1)
        .width('100%')
        .justifyContent(FlexAlign.Center)
        .alignItems(HorizontalAlign.Center)
      } else {
        List({ space: 10 }) {
          ForEach(this.connections, (conn: SavedConnection) => {
            ListItem() {
              this.connectionRow(conn)
            }
          }, (conn: SavedConnection) => `${conn.id}#${conn.updatedAt}`)
        }
        .layoutWeight(1)
        .width('100%')
        .padding({ left: 24, right: 24, top: 14, bottom: 24 })
        .scrollBar(BarState.Auto)
      }
      }
      .width('100%')
      .height('100%')

      Row() {
        Image($r('app.media.ic_plus'))
          .width(24)
          .height(24)
      }
      .width(56)
      .height(56)
      .borderRadius(28)
      .backgroundColor($r('app.color.accent'))
      .justifyContent(FlexAlign.Center)
      .alignItems(VerticalAlign.Center)
      .scale({
        x: this.fabPressed ? 0.9 : (this.fabHovered ? 1.06 : 1.0),
        y: this.fabPressed ? 0.9 : (this.fabHovered ? 1.06 : 1.0)
      })
      .shadow({ radius: 18, color: '#33000000', offsetX: 0, offsetY: 6 })
      .animation({ duration: 140, curve: Curve.FastOutSlowIn })
      .margin({ right: 24, bottom: 24 })
      .onHover((isHover: boolean) => {
        this.fabHovered = isHover;
      })
      .onMouse((event: MouseEvent) => {
        if (event.action === MouseAction.Press) {
          this.fabPressed = true;
        } else if (event.action === MouseAction.Release) {
          this.fabPressed = false;
        }
      })
      .onClick(() => {
        this.openEditor('');
      })
    }
    .width('100%')
    .height('100%')
    .backgroundColor($r('app.color.page_bg'))
  }
}
