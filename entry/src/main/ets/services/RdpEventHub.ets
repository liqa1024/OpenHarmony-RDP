import { onEvent, RdpEventCallback, RdpEventPayload } from 'libhmrdp.so';
import { RdpEvent } from '../model/RdpModels';

export type RdpEventHandler = (event: RdpEvent, payload: RdpEventPayload) => void;

class RdpEventHubImpl {
  private handlers: Map<number, RdpEventHandler> = new Map<number, RdpEventHandler>();
  private subscribed: boolean = false;

  register(handle: number, handler: RdpEventHandler): void {
    if (!this.subscribed) {
      this.subscribed = true;
      const callback: RdpEventCallback = (handle: number, event: number,
        payload: RdpEventPayload): void => {
        const target: RdpEventHandler | undefined = this.handlers.get(handle);
        if (target !== undefined) {
          target(event as RdpEvent, payload);
        }
      };
      onEvent(callback);
    }
    this.handlers.set(handle, handler);
  }

  unregister(handle: number): void {
    this.handlers.delete(handle);
  }
}

export const RdpEventHub: RdpEventHubImpl = new RdpEventHubImpl();
