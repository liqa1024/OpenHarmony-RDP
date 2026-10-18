@Component
export struct PageBackground {
  @Prop uri: string = '';

  build() {
    Stack() {
      if (this.uri.length > 0) {
        Image(this.uri)
          .width('100%')
          .height('100%')
          .objectFit(ImageFit.Cover)
          .draggable(false)
          .hitTestBehavior(HitTestMode.None)
        Column()
          .width('100%')
          .height('100%')
          .backgroundColor($r('app.color.scrim_wallpaper'))
          .hitTestBehavior(HitTestMode.None)
      } else {
        Column()
          .width('100%')
          .height('100%')
          .linearGradient({
            angle: 135,
            colors: [
              [$r('app.color.bg_canvas'), 0],
              [$r('app.color.bg_canvas_accent'), 1]
            ]
          })
          .hitTestBehavior(HitTestMode.None)
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor($r('app.color.bg_canvas'))
    .hitTestBehavior(HitTestMode.None)
  }
}
