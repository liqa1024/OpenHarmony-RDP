export function fitPictureRect(containerWidth: number, containerHeight: number,
  contentWidth: number, contentHeight: number): number[] {
  const cw: number = containerWidth > 0 ? containerWidth : 1;
  const ch: number = containerHeight > 0 ? containerHeight : 1;
  if (contentWidth <= 0 || contentHeight <= 0) {
    return [0, 0, cw, ch];
  }
  const contentAspect: number = contentWidth / contentHeight;
  const containerAspect: number = cw / ch;
  if (contentAspect > containerAspect) {
    const height: number = cw / contentAspect;
    return [0, (ch - height) / 2, cw, height];
  }
  const width: number = ch * contentAspect;
  return [(cw - width) / 2, 0, width, ch];
}

export function mapPicturePoint(x: number, y: number, pictureWidth: number, pictureHeight: number,
  contentWidth: number, contentHeight: number): number[] {
  const pw: number = pictureWidth > 0 ? pictureWidth : 1;
  const ph: number = pictureHeight > 0 ? pictureHeight : 1;
  const maxX: number = contentWidth > 0 ? contentWidth - 1 : 0;
  const maxY: number = contentHeight > 0 ? contentHeight - 1 : 0;
  const normalizedX: number = Math.min(1, Math.max(0, x / pw));
  const normalizedY: number = Math.min(1, Math.max(0, y / ph));
  return [Math.round(normalizedX * maxX), Math.round(normalizedY * maxY)];
}
