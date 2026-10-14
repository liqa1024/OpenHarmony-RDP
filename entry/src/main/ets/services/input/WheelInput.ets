import { defaultDisplay } from '../../utils/DisplayUtil';
import { TouchpadWheelMapper, WheelChunk, WHEEL_VP_PER_DEGREE } from '../TouchpadWheel';
import { InputSink } from './InputSink';
import { InputGeometry, isPinchEvent } from './InputPolicy';

// Axial scroll routing: engine frames, touchpad two-finger, and mouse wheel fold into one
// rotation口径 through this mapper pair; frame ownership is decided per event, never latched.
export class WheelInput {
  private sink: InputSink;
  private geometry: InputGeometry;
  private touchpadWheel: TouchpadWheelMapper = new TouchpadWheelMapper();
  private wheelMapper: TouchpadWheelMapper = new TouchpadWheelMapper();
  private engineEnabled: boolean = false;
  private engineTouchpad: boolean = false;
  private engineAxisAt: number = 0;
  private directContactAt: number = 0;
  private wheelPxPerVp: number = 0;
  private wheelChunks: WheelChunk[] = [];
  private isTouchpad: (deviceId: number | undefined, sourceTool: SourceTool | undefined) => boolean;
  private isPinchActive: () => boolean;
  private isTouchpadPan: () => boolean;

  constructor(sink: InputSink, geometry: InputGeometry,
    isTouchpad: (deviceId: number | undefined, sourceTool: SourceTool | undefined) => boolean,
    isPinchActive: () => boolean, isTouchpadPan: () => boolean) {
    this.sink = sink;
    this.geometry = geometry;
    this.isTouchpad = isTouchpad;
    this.isPinchActive = isPinchActive;
    this.isTouchpadPan = isTouchpadPan;
  }

  setEngineEnabled(enabled: boolean): void {
    this.engineEnabled = enabled;
  }

  setTouchpadScrollSpeed(scale: number): void {
    this.touchpadWheel.setSpeed(scale);
  }

  // A direct touch contact owns whatever scroll gesture follows it, whatever the touch path then
  // does with it — the engine must not take that gesture.
  noteDirectContact(): void {
    this.directContactAt = Date.now();
  }

  // Frames carry neither position nor source: the wheel/touchpad split comes from the raw axis events.
  noteAxisEvent(event: AxisEvent): void {
    this.engineTouchpad = this.isTouchpad(event.deviceId, event.sourceTool);
    this.engineAxisAt = Date.now();
  }

  // Per frame, on input state the next event refreshes — never latched to a gesture, which the engine
  // can end silently. A direct touch contact, a pinch, or a touchpad gesture the pan channel took
  // over owns the frames instead.
  engineAcceptsFrame(): boolean {
    return !(this.directContactAt > this.engineAxisAt) && !this.isPinchActive()
      && !(this.isTouchpadPan() && this.engineTouchpad);
  }

  // The engine owns the wheel axis; a pan-mode touchpad gesture and pinch keep to the shared pipeline.
  consumesAxis(event: AxisEvent): boolean {
    if (!this.engineEnabled || isPinchEvent(event)) {
      return false;
    }
    if (this.isTouchpadPan() && this.isTouchpad(event.deviceId, event.sourceTool)) {
      return false;
    }
    return (event.axisVertical ?? 0) !== 0 || (event.axisHorizontal ?? 0) !== 0;
  }

  // The overlay reports (x, y) in vp, one component per axis overlay; the mapper takes (vertical,
  // horizontal).
  handleEngineScroll(dx: number, dy: number, x: number, y: number): void {
    this.handleWheelScroll(this.engineTouchpad ? this.touchpadWheel : this.wheelMapper,
      dy, dx, [x, y]);
  }

  beginAxis(): void {
    this.touchpadWheel.reset();
    this.wheelMapper.reset();
  }

  scroll(vertical: number, horizontal: number, point: number[], touchpad: boolean): void {
    if (touchpad) {
      // Touchpad axis is content px; the mapper is vp, so fold the density in here.
      const pxPerVp: number = this.pxPerVp();
      this.handleWheelScroll(this.touchpadWheel, vertical / pxPerVp, horizontal / pxPerVp, point);
    } else {
      // Wheel axis is in degrees; ArkUI's degree -> vp step lands it in the mapper's vp口径 directly.
      // The host wheel-speed setting rides in the axis value; the remote follows it.
      this.handleWheelScroll(this.wheelMapper, vertical * WHEEL_VP_PER_DEGREE,
        horizontal * WHEEL_VP_PER_DEGREE, point);
    }
  }

  private handleWheelScroll(mapper: TouchpadWheelMapper, vertical: number, horizontal: number,
    point: number[]): void {
    mapper.setPxPerVp(this.pxPerVp());
    this.wheelChunks.length = 0;
    mapper.map(vertical, horizontal, this.wheelChunks);
    let wheelV: number = 0;
    let wheelH: number = 0;
    for (let i = 0; i < this.wheelChunks.length; i++) {
      const chunk: WheelChunk = this.wheelChunks[i];
      const amount: number = chunk.negative ? -chunk.rotation : chunk.rotation;
      if (chunk.horizontal) {
        wheelH += amount;
      } else {
        wheelV += amount;
      }
    }
    if (wheelV !== 0 || wheelH !== 0) {
      this.sink.scroll(wheelV, wheelH, point[0], point[1]);
    }
  }

  private pxPerVp(): number {
    if (this.wheelPxPerVp <= 0) {
      const disp = defaultDisplay();
      this.wheelPxPerVp = disp !== undefined && disp.densityPixels > 0 ? disp.densityPixels : 1;
    }
    return this.wheelPxPerVp;
  }
}
