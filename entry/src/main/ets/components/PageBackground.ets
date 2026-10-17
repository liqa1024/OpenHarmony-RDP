// Full-page wallpaper layer shared by the main list, settings and app pages: the
// image stays behind the content and a theme-coloured veil keeps text contrast on
// arbitrary photos. The same URI is passed on every page, so the image cache serves
// the decoded frame while only the page content changes.
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
          .backgroundColor($r('app.color.page_bg'))
          .opacity(0.25)
          .hitTestBehavior(HitTestMode.None)
      }
    }
    .width('100%')
    .height('100%')
    .hitTestBehavior(HitTestMode.None)
  }
}
