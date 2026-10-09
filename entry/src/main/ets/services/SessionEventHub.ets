import { onEvent, RdpEventCallback, RdpEventPayload } from 'libhmrdp.so';
import { RdpEvent } from '../model/RdpModels';

export type SessionEventHandler = (event: RdpEvent, payload: RdpEventPayload) => void;

class SessionEventHubImpl {
  private handlers: Map<number, SessionEventHandler> = new Map<number, SessionEventHandler>();
  private subscribed: boolean = false;

  register(handle: number, handler: SessionEventHandler): void {
    if (!this.subscribed) {
      this.subscribed = true;
      const callback: RdpEventCallback = (handle: number, event: number,
        payload: RdpEventPayload): void => {
        const target: SessionEventHandler | undefined = this.handlers.get(handle);
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

export const SessionEventHub: SessionEventHubImpl = new SessionEventHubImpl();
